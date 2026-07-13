#define MCP_LOG_COMPONENT "http"

#include "http_endpoint.hpp"

#include <algorithm>
#include <cstdlib>

#include <mcp/log.hpp>
#include <mcp/types.hpp>

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
    pal::ignore_broken_pipe_signals();  // Dead peers surface as write errors.

    listen_fd_ = pal::tcp_listen(options_.host, options_.port, error);
    if (listen_fd_ == pal::kInvalidFd) {
        running_.store(false);
        return false;
    }
    if (!wake_.valid()) {
        pal::close_fd(listen_fd_);
        running_.store(false);
        error = "failed to create wake event";
        return false;
    }
    bound_port_.store(pal::tcp_local_port(listen_fd_));
    MCP_LOG(info, "listening on " << options_.host << ":" << bound_port_.load()
                                  << " (path " << options_.path << ")");
    accept_thread_ = mcp::sys::thread([this] { accept_loop(); });
    return true;
}

void HttpEndpoint::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    wake_.signal();
    pal::shutdown_fd(listen_fd_);
    {
        std::lock_guard<mcp::sys::mutex> lock(conn_mutex_);
        for (const pal::fd_t fd : conn_fds_) {
            pal::shutdown_fd(fd);
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    std::vector<mcp::sys::thread> threads;
    {
        std::lock_guard<mcp::sys::mutex> lock(conn_mutex_);
        threads.swap(conn_threads_);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    {
        std::lock_guard<mcp::sys::mutex> lock(conn_mutex_);
        for (pal::fd_t fd : conn_fds_) {
            pal::close_fd(fd);
        }
        conn_fds_.clear();
    }
    pal::close_fd(listen_fd_);
}

void HttpEndpoint::accept_loop() {
    while (running_.load()) {
        const int ready = pal::poll_readable(listen_fd_, &wake_, -1);
        if (ready <= 0 || !running_.load()) {
            return;
        }
        const pal::fd_t fd = pal::tcp_accept(listen_fd_);
        if (fd == pal::kInvalidFd) {
            continue;
        }
        std::lock_guard<mcp::sys::mutex> lock(conn_mutex_);
        if (!running_.load()) {
            pal::fd_t doomed = fd;
            pal::close_fd(doomed);
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

void HttpEndpoint::write_simple(std::intptr_t fd, int status, const std::string& reason,
                                const std::string& body,
                                const std::string& content_type,
                                const HeaderList& extra_headers) {
    HeaderList headers{{"Content-Type", content_type},
                       {"MCP-Protocol-Version", kProtocolVersion},
                       {"Connection", "close"}};
    for (const auto& header : extra_headers) {
        headers.push_back(header);
    }
    if (status >= 400) {
        MCP_LOG(warn, "HTTP " << status << " " << reason
                              << (body.empty() ? "" : ": ") << body);
    } else {
        MCP_LOG(debug, "HTTP " << status << " " << reason << " ("
                               << body.size() << " bytes)");
    }
    const auto payload = serialize_response(status, reason, headers, body);
    (void)pal::write_all(fd, payload.data(), payload.size());
}

void HttpEndpoint::handle_connection(std::intptr_t fd) {
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
            if (pal::poll_readable(fd, &wake_, 30000) != 1 ||
                !running_.load()) {
                break;  // shutdown or idle timeout
            }
            char chunk[8192];
            const long n = pal::read_some(fd, chunk, sizeof(chunk));
            if (n <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(n));
        }
        if (!have_head) {
            break;
        }

        MCP_LOG(info, head.method << " " << head.target);
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
            if (pal::poll_readable(fd, &wake_, 30000) != 1 ||
                !running_.load()) {
                truncated = true;
                break;
            }
            char chunk[8192];
            const long n = pal::read_some(fd, chunk, sizeof(chunk));
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
    pal::shutdown_fd(fd);
    std::lock_guard<mcp::sys::mutex> lock(conn_mutex_);
    const auto it = std::find(conn_fds_.begin(), conn_fds_.end(), fd);
    if (it != conn_fds_.end()) {
        conn_fds_.erase(it);
        pal::fd_t doomed = fd;
        pal::close_fd(doomed);
    }
}

}  // namespace mcp::detail
