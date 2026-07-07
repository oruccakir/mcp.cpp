#include <mcp/transport/http_server_transport.hpp>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <mcp/types.hpp>

#include "http/http_codec.hpp"
#include "http/socket_util.hpp"
#include "line_io.hpp"

namespace mcp {

namespace {

/// Captures responses produced while dispatching one POST's messages.
/// Dispatch is synchronous on the connection thread (see Session), so a
/// thread_local is a correct correlation mechanism (FR-TRAN-006).
struct PostCapture {
    bool active = false;
    std::vector<json> responses;
};
thread_local PostCapture t_post_capture;

bool is_localhost_origin(const std::string& origin) {
    return origin.rfind("http://localhost", 0) == 0 ||
           origin.rfind("http://127.0.0.1", 0) == 0 ||
           origin.rfind("https://localhost", 0) == 0 ||
           origin.rfind("https://127.0.0.1", 0) == 0;
}

}  // namespace

HttpServerTransport::HttpServerTransport(HttpServerOptions options)
    : options_(std::move(options)) {}

HttpServerTransport::~HttpServerTransport() { disconnect(); }

void HttpServerTransport::set_message_handler(std::function<void(Message)> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    message_handler_ = std::move(handler);
}
void HttpServerTransport::set_error_handler(std::function<void(Error)> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    error_handler_ = std::move(handler);
}
void HttpServerTransport::set_close_handler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    close_handler_ = std::move(handler);
}

void HttpServerTransport::emit_error(const Error& error) {
    std::function<void(Error)> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = error_handler_;
    }
    if (handler) {
        handler(error);
    }
}

void HttpServerTransport::connect() {
    if (running_.exchange(true)) {
        return;
    }
    ::signal(SIGPIPE, SIG_IGN);  // Dead peers surface as write errors.

    std::string error;
    listen_fd_ = detail::listen_tcp(options_.host, options_.port, error);
    if (listen_fd_ < 0) {
        running_.store(false);
        emit_error(Error(ErrorCode::InternalError, "http listen failed: " + error));
        return;
    }
    if (::pipe(wake_pipe_) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_.store(false);
        emit_error(Error(ErrorCode::InternalError, "failed to create wake pipe"));
        return;
    }
    bound_port_.store(detail::local_port(listen_fd_));
    accept_thread_ = std::thread([this] { accept_loop(); });
}

void HttpServerTransport::disconnect() {
    if (!running_.exchange(false)) {
        return;
    }
    // Wake everything: pollers see the wake byte; SSE loops see running_.
    if (wake_pipe_[1] >= 0) {
        const char byte = 0;
        (void)detail::write_all(wake_pipe_[1], &byte, 1);
    }
    sse_cv_.notify_all();
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

    if (!close_emitted_.exchange(true)) {
        std::function<void()> handler;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler = close_handler_;
        }
        if (handler) {
            handler();
        }
    }
}

void HttpServerTransport::accept_loop() {
    while (running_.load()) {
        const int ready = detail::poll_readable(listen_fd_, wake_pipe_[0], -1);
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

void HttpServerTransport::handle_connection(int fd) {
    serve_connection(fd);
    // Close eagerly so dropped SSE clients see EOF and can reconnect
    // (FR-TRAN-007); disconnect() only owns fds still in the list.
    ::shutdown(fd, SHUT_RDWR);
    std::lock_guard<std::mutex> lock(conn_mutex_);
    const auto it = std::find(conn_fds_.begin(), conn_fds_.end(), fd);
    if (it != conn_fds_.end()) {
        conn_fds_.erase(it);
        ::close(fd);
    }
}

void HttpServerTransport::serve_connection(int fd) {
    // Read one request (Connection: close semantics; one request per socket).
    std::string buffer;
    detail::HttpHead head;
    std::size_t consumed = 0;
    std::string parse_error;

    for (;;) {
        if (parse_head(buffer, /*request_mode=*/true, head, consumed,
                       parse_error)) {
            break;
        }
        if (!parse_error.empty()) {
            write_simple(fd, 400, "Bad Request", "malformed HTTP request");
            return;
        }
        if (detail::poll_readable(fd, wake_pipe_[0], 30000) != 1 ||
            !running_.load()) {
            return;  // shutdown or idle timeout
        }
        char chunk[8192];
        const long n = detail::read_some(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            return;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
    }

    // Route.
    if (head.target != options_.path) {
        write_simple(fd, 404, "Not Found", "unknown path");
        return;
    }
    if (!origin_allowed(head)) {
        write_simple(fd, 403, "Forbidden", "origin not allowed");  // FR-TRAN-008
        return;
    }
    if (!authorized(head)) {
        write_simple(fd, 401, "Unauthorized", "authorization failed");
        return;
    }

    if (head.method == "POST") {
        // Assemble the body (Content-Length framing).
        const std::size_t length = static_cast<std::size_t>(
            std::strtoull(head.header("content-length").c_str(), nullptr, 10));
        std::string body = buffer.substr(consumed);
        while (body.size() < length) {
            if (detail::poll_readable(fd, wake_pipe_[0], 30000) != 1 ||
                !running_.load()) {
                return;
            }
            char chunk[8192];
            const long n = detail::read_some(fd, chunk, sizeof(chunk));
            if (n <= 0) {
                return;
            }
            body.append(chunk, static_cast<std::size_t>(n));
        }
        handle_post(fd, head, body);
        return;
    }
    if (head.method == "GET") {
        handle_get(fd, head);
        return;
    }
    write_simple(fd, 405, "Method Not Allowed", "use POST or GET");
}

bool HttpServerTransport::origin_allowed(const detail::HttpHead& head) const {
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

bool HttpServerTransport::authorized(const detail::HttpHead& head) const {
    return !options_.authorize || options_.authorize(head.headers);
}

void HttpServerTransport::write_simple(int fd, int status,
                                       const std::string& reason,
                                       const std::string& body,
                                       const std::string& content_type) {
    const auto payload = detail::serialize_response(
        status, reason,
        {{"Content-Type", content_type},
         {"MCP-Protocol-Version", kProtocolVersion},
         {"Connection", "close"}},
        body);
    (void)detail::write_all(fd, payload.data(), payload.size());
}

void HttpServerTransport::handle_post(int fd, const detail::HttpHead&,
                                      const std::string& body) {
    auto frame = parse_frame(body);
    if (!frame) {
        // 400 with a null-id JSON-RPC error body (FR-TRAN-006).
        JsonRpcResponse response;
        response.error = frame.error();
        write_simple(fd, 400, "Bad Request", serialize_message(Message(response)),
                     "application/json");
        emit_error(frame.error());
        return;
    }

    std::function<void(Message)> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = message_handler_;
    }

    t_post_capture.active = true;
    t_post_capture.responses.clear();
    // Invalid batch items are answered individually with null-id errors.
    for (const auto& item_error : frame.value().item_errors) {
        JsonRpcResponse response;
        response.error = item_error;
        t_post_capture.responses.push_back(message_to_json(Message(response)));
    }
    if (handler) {
        for (auto& message : frame.value().messages) {
            handler(std::move(message));
        }
    }
    std::vector<json> responses = std::move(t_post_capture.responses);
    t_post_capture.active = false;
    t_post_capture.responses.clear();

    if (responses.empty()) {
        // Only notifications/responses: 202 Accepted, no body (FR-TRAN-006).
        const auto payload = detail::serialize_response(
            202, "Accepted",
            {{"MCP-Protocol-Version", kProtocolVersion},
             {"Connection", "close"}},
            "");
        (void)detail::write_all(fd, payload.data(), payload.size());
        return;
    }

    std::string body_out;
    if (frame.value().was_batch) {
        json arr = json::array();
        for (auto& response : responses) {
            arr.push_back(std::move(response));
        }
        body_out = arr.dump();
    } else {
        body_out = responses.front().dump();
    }
    write_simple(fd, 200, "OK", body_out, "application/json");
}

void HttpServerTransport::handle_get(int fd, const detail::HttpHead& head) {
    if (!head.header_contains("accept", "text/event-stream")) {
        write_simple(fd, 406, "Not Acceptable", "expected text/event-stream");
        return;
    }

    std::uint64_t my_generation;
    std::uint64_t cursor;
    std::uint64_t last_assigned;
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        my_generation = ++stream_generation_;  // newest stream wins
        last_assigned = next_event_id_ - 1;
        const std::string last_seen = head.header("last-event-id");
        if (!last_seen.empty()) {
            // Resume after the given id (FR-TRAN-009).
            cursor = std::strtoull(last_seen.c_str(), nullptr, 10) + 1;
        } else {
            // Fresh stream: start at the first never-delivered event so
            // messages queued while no stream was attached are not lost.
            cursor = next_undelivered_;
        }
    }
    sse_cv_.notify_all();

    const auto header = detail::serialize_response(
        200, "OK",
        {{"Content-Type", "text/event-stream"},
         {"Cache-Control", "no-cache"},
         {"Connection", "keep-alive"},
         {"MCP-Protocol-Version", kProtocolVersion}},
        "", /*streaming=*/true);
    if (!detail::write_all(fd, header.data(), header.size())) {
        return;
    }

    // Initial event: current id baseline + retry hint (FR-TRAN-007).
    detail::SseEvent initial;
    initial.id = std::to_string(last_assigned);
    initial.retry_ms = options_.sse_retry_ms;
    const auto initial_text = detail::format_sse_event(initial);
    if (!detail::write_all(fd, initial_text.data(), initial_text.size())) {
        return;
    }

    for (;;) {
        std::vector<SseItem> pending;
        {
            std::unique_lock<std::mutex> lock(sse_mutex_);
            sse_cv_.wait(lock, [&] {
                return !running_.load() || stream_generation_ != my_generation ||
                       (!ring_.empty() && ring_.back().id >= cursor);
            });
            if (!running_.load() || stream_generation_ != my_generation) {
                return;  // shutdown or a newer stream took over
            }
            for (const auto& item : ring_) {
                if (item.id >= cursor) {
                    pending.push_back(item);
                }
            }
            if (!pending.empty()) {
                cursor = pending.back().id + 1;
                if (cursor > next_undelivered_) {
                    next_undelivered_ = cursor;
                }
            }
        }
        for (const auto& item : pending) {
            detail::SseEvent event;
            event.id = std::to_string(item.id);
            event.data = item.data;
            const auto text = detail::format_sse_event(event);
            if (!detail::write_all(fd, text.data(), text.size())) {
                return;  // client went away; it may reconnect with Last-Event-ID
            }
        }
    }
}

void HttpServerTransport::enqueue_sse(const std::string& payload) {
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        ring_.push_back(SseItem{next_event_id_++, payload});
        while (ring_.size() > options_.replay_buffer_size) {
            ring_.pop_front();
        }
    }
    sse_cv_.notify_all();
}

void HttpServerTransport::route_send(const Message& message) {
    // Responses produced while a POST dispatch is running on this thread
    // belong to that POST; everything else goes to the SSE stream.
    if (t_post_capture.active &&
        std::holds_alternative<JsonRpcResponse>(message)) {
        t_post_capture.responses.push_back(message_to_json(message));
        return;
    }
    enqueue_sse(serialize_message(message));
}

void HttpServerTransport::send(const Message& message) { route_send(message); }

void HttpServerTransport::send_batch(const std::vector<Message>& messages) {
    for (const auto& message : messages) {
        route_send(message);
    }
}

}  // namespace mcp
