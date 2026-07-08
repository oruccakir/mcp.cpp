#include "http_endpoint.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <mcp/types.hpp>

#include "../line_io.hpp"
#include "socket_util.hpp"

namespace mcp::detail {

namespace {

bool is_localhost_origin(const std::string& origin) {
    return origin.rfind("http://localhost", 0) == 0 ||
           origin.rfind("http://127.0.0.1", 0) == 0 ||
           origin.rfind("https://localhost", 0) == 0 ||
           origin.rfind("https://127.0.0.1", 0) == 0;
}

}  // namespace

HttpEndpoint::HttpEndpoint(HttpServerOptions options, RequestHandler handler)
    : options_(std::move(options)), handler_(std::move(handler)) {}

HttpEndpoint::~HttpEndpoint() { stop(); }

bool HttpEndpoint::start(std::string& error) {
    if (running_.exchange(true)) {
        return true;
    }
    ::signal(SIGPIPE, SIG_IGN);  // Dead peers surface as write errors.

    listen_fd_ = listen_tcp(options_.host, options_.port, error);
    if (listen_fd_ < 0) {
        running_.store(false);
        return false;
    }
    if (::pipe(wake_pipe_) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_.store(false);
        error = "failed to create wake pipe";
        return false;
    }
    bound_port_.store(local_port(listen_fd_));
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void HttpEndpoint::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (wake_pipe_[1] >= 0) {
        const char byte = 0;
        (void)write_all(wake_pipe_[1], &byte, 1);
    }
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
    }
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (const int fd : conn_fds_) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        threads.swap(conn_threads_);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (const int fd : conn_fds_) {
            ::close(fd);
        }
        conn_fds_.clear();
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    for (int* fd : {&wake_pipe_[0], &wake_pipe_[1]}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
}

void HttpEndpoint::accept_loop() {
    while (running_.load()) {
        const int ready = poll_readable(listen_fd_, wake_pipe_[0], -1);
        if (ready <= 0 || !running_.load()) {
            return;
        }
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (!running_.load()) {
            ::close(fd);
            return;
        }
        conn_fds_.push_back(fd);
        conn_threads_.emplace_back([this, fd] { handle_connection(fd); });
    }
}

bool HttpEndpoint::origin_allowed(const HttpHead& head) const {
    const std::string origin = head.header("origin");
    if (origin.empty() || is_localhost_origin(origin)) {
        return true;
    }
    for (const auto& allowed : options_.allowed_origins) {
        if (origin == allowed) {
            return true;
        }
    }
    return false;
}

void HttpEndpoint::write_simple(int fd, int status, const std::string& reason,
                                const std::string& body,
                                const std::string& content_type,
                                const HeaderList& extra_headers) {
    HeaderList headers{{"Content-Type", content_type},
                       {"MCP-Protocol-Version", kProtocolVersion},
                       {"Connection", "close"}};
    for (const auto& header : extra_headers) {
        headers.push_back(header);
    }
    const auto payload = serialize_response(status, reason, headers, body);
    (void)write_all(fd, payload.data(), payload.size());
}

void HttpEndpoint::handle_connection(int fd) {
    // One request per connection (Connection: close semantics).
    do {
        std::string buffer;
        HttpHead head;
        std::size_t consumed = 0;
        std::string parse_error;
        bool have_head = false;

        while (!have_head) {
            have_head =
                parse_head(buffer, /*request_mode=*/true, head, consumed,
                           parse_error);
            if (have_head) {
                break;
            }
            if (!parse_error.empty()) {
                write_simple(fd, 400, "Bad Request", "malformed HTTP request");
                break;
            }
            if (poll_readable(fd, wake_pipe_[0], 30000) != 1 ||
                !running_.load()) {
                break;  // shutdown or idle timeout
            }
            char chunk[8192];
            const long n = read_some(fd, chunk, sizeof(chunk));
            if (n <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(n));
        }
        if (!have_head) {
            break;
        }

        if (head.target != options_.path) {
            write_simple(fd, 404, "Not Found", "unknown path");
            break;
        }
        if (!origin_allowed(head)) {
            // FR-TRAN-008.
            write_simple(fd, 403, "Forbidden", "origin not allowed");
            break;
        }
        if (options_.authorize && !options_.authorize(head.headers)) {
            write_simple(fd, 401, "Unauthorized", "authorization failed");
            break;
        }

        // Assemble the body (Content-Length framing; GET/DELETE have none).
        const std::size_t length = static_cast<std::size_t>(
            std::strtoull(head.header("content-length").c_str(), nullptr, 10));
        std::string body = buffer.substr(consumed);
        bool truncated = false;
        while (body.size() < length) {
            if (poll_readable(fd, wake_pipe_[0], 30000) != 1 ||
                !running_.load()) {
                truncated = true;
                break;
            }
            char chunk[8192];
            const long n = read_some(fd, chunk, sizeof(chunk));
            if (n <= 0) {
                truncated = true;
                break;
            }
            body.append(chunk, static_cast<std::size_t>(n));
        }
        if (truncated) {
            break;
        }

        handler_(head, body, fd);
    } while (false);

    // Close eagerly so dropped SSE clients see EOF and can reconnect
    // (FR-TRAN-007); stop() only owns fds still in the list.
    ::shutdown(fd, SHUT_RDWR);
    std::lock_guard<std::mutex> lock(conn_mutex_);
    const auto it = std::find(conn_fds_.begin(), conn_fds_.end(), fd);
    if (it != conn_fds_.end()) {
        conn_fds_.erase(it);
        ::close(fd);
    }
}

}  // namespace mcp::detail
