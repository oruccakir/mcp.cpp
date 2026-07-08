#include <mcp/transport/http_client_transport.hpp>

#include <cstdlib>

#include <mcp/types.hpp>

#include "../platform/pal.hpp"
#include "http/http_codec.hpp"

namespace mcp {

HttpClientTransport::HttpClientTransport(HttpClientOptions options)
    : options_(std::move(options)), retry_ms_(options_.reconnect_delay_ms) {}

HttpClientTransport::~HttpClientTransport() { disconnect(); }

void HttpClientTransport::set_message_handler(std::function<void(Message)> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    message_handler_ = std::move(handler);
}
void HttpClientTransport::set_error_handler(std::function<void(Error)> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    error_handler_ = std::move(handler);
}
void HttpClientTransport::set_close_handler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    close_handler_ = std::move(handler);
}

void HttpClientTransport::emit_error(const Error& error) {
    std::function<void(Error)> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = error_handler_;
    }
    if (handler) {
        handler(error);
    }
}

void HttpClientTransport::emit_close() {
    if (close_emitted_.exchange(true)) {
        return;
    }
    std::function<void()> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = close_handler_;
    }
    if (handler) {
        handler();
    }
}

std::string HttpClientTransport::session_id() const {
    std::lock_guard<std::mutex> lock(
        const_cast<std::mutex&>(state_mutex_));
    return session_id_;
}

void HttpClientTransport::send_session_delete() {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        sid = session_id_;
    }
    if (sid.empty()) {
        return;
    }
    std::string error;
    pal::fd_t fd = pal::tcp_connect(options_.host, options_.port, 1000, error);
    if (fd == pal::kInvalidFd) {
        return;
    }
    const auto request = detail::serialize_request(
        "DELETE", options_.path,
        {{"Host", options_.host + ":" + std::to_string(options_.port)},
         {"Mcp-Session-Id", sid},
         {"MCP-Protocol-Version", kProtocolVersion},
         {"Connection", "close"}},
        "");
    if (pal::write_all(fd, request.data(), request.size())) {
        char buffer[512];
        (void)pal::poll_readable(fd, nullptr, 500);
        (void)pal::read_some(fd, buffer, sizeof(buffer));
    }
    pal::close_fd(fd);
}

void HttpClientTransport::connect() {
    if (running_.exchange(true)) {
        return;
    }
    pal::ignore_broken_pipe_signals();
    wake_ = std::make_unique<pal::WakeEvent>();
    if (!wake_->valid()) {
        running_.store(false);
        emit_error(Error(ErrorCode::InternalError, "failed to create wake event"));
        return;
    }
    if (options_.open_sse_stream) {
        sse_thread_ = std::thread([this] { sse_loop(); });
    }
}

void HttpClientTransport::disconnect() {
    if (!running_.exchange(false)) {
        return;
    }
    send_session_delete();  // best-effort session termination
    if (wake_) {
        wake_->signal();
    }
    if (sse_thread_.joinable() &&
        sse_thread_.get_id() != std::this_thread::get_id()) {
        sse_thread_.join();
    }
    wake_.reset();
}

void HttpClientTransport::deliver_frame(const std::string& payload) {
    if (payload.empty()) {
        return;  // e.g. the server's initial empty SSE event
    }
    auto frame = parse_frame(payload);
    if (!frame) {
        emit_error(frame.error());
        return;
    }
    for (const auto& item_error : frame.value().item_errors) {
        emit_error(item_error);
    }
    std::function<void(Message)> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = message_handler_;
    }
    if (!handler) {
        return;
    }
    for (auto& message : frame.value().messages) {
        handler(std::move(message));
    }
}

void HttpClientTransport::send(const Message& message) {
    post_payload(serialize_message(message));
}

void HttpClientTransport::send_batch(const std::vector<Message>& messages) {
    post_payload(serialize_batch(messages));
}

void HttpClientTransport::post_payload(const std::string& body) {
    std::string error;
    pal::fd_t fd = pal::tcp_connect(options_.host, options_.port,
                                    options_.connect_timeout_ms, error);
    if (fd == pal::kInvalidFd) {
        emit_error(Error(ErrorCode::InternalError, "http connect failed: " + error));
        return;
    }

    detail::HeaderList headers{
        {"Host", options_.host + ":" + std::to_string(options_.port)},
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", kProtocolVersion},  // FR-TRAN-008
        {"Connection", "close"},
    };
    if (options_.origin) {
        headers.emplace_back("Origin", *options_.origin);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!session_id_.empty()) {
            headers.emplace_back("Mcp-Session-Id", session_id_);
        }
    }
    for (const auto& [name, value] : options_.headers) {
        headers.emplace_back(name, value);
    }
    const auto request =
        detail::serialize_request("POST", options_.path, headers, body);
    if (!pal::write_all(fd, request.data(), request.size())) {
        pal::close_fd(fd);
        emit_error(Error(ErrorCode::InternalError, "http write failed"));
        return;
    }

    // Read the response head.
    std::string buffer;
    detail::HttpHead head;
    std::size_t consumed = 0;
    std::string parse_error;
    bool have_head = false;
    while (!have_head) {
        if (pal::poll_readable(fd, wake_.get(), 30000) != 1 ||
            !running_.load()) {
            pal::close_fd(fd);
            return;
        }
        char chunk[8192];
        const long n = pal::read_some(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            pal::close_fd(fd);
            emit_error(Error(ErrorCode::InternalError,
                             "connection closed before HTTP response"));
            return;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
        have_head = detail::parse_head(buffer, /*request_mode=*/false, head,
                                       consumed, parse_error);
        if (!have_head && !parse_error.empty()) {
            pal::close_fd(fd);
            emit_error(Error(ErrorCode::InternalError,
                             "malformed HTTP response: " + parse_error));
            return;
        }
    }
    buffer.erase(0, consumed);

    // Capture the server-assigned session id (Mcp-Session-Id).
    if (const auto sid = head.header("mcp-session-id"); !sid.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_id_ = sid;
    }

    if (head.status == 202) {
        pal::close_fd(fd);
        return;  // notification/response accepted (FR-TRAN-006)
    }
    if (head.status != 200) {
        pal::close_fd(fd);
        std::string hint;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (head.status == 404 && !session_id_.empty()) {
                hint = " (session expired - reinitialize)";
                session_id_.clear();
            }
        }
        emit_error(Error(ErrorCode::InternalError,
                         "HTTP " + std::to_string(head.status) + " " +
                             head.reason + hint));
        return;
    }

    const bool is_sse = head.header_contains("content-type", "text/event-stream");
    const bool is_chunked =
        head.header_contains("transfer-encoding", "chunked");
    const std::string length_header = head.header("content-length");

    if (is_sse) {
        // Streaming POST response (FR-TRAN-006): deliver each event as it
        // arrives until the server closes the stream.
        detail::SseParser parser;
        std::vector<detail::SseEvent> events;
        parser.feed(buffer.data(), buffer.size(), events);
        for (;;) {
            for (const auto& event : events) {
                deliver_frame(event.data);
            }
            events.clear();
            if (pal::poll_readable(fd, wake_.get(), 30000) != 1 ||
                !running_.load()) {
                break;
            }
            char chunk[8192];
            const long n = pal::read_some(fd, chunk, sizeof(chunk));
            if (n <= 0) {
                break;
            }
            parser.feed(chunk, static_cast<std::size_t>(n), events);
        }
        pal::close_fd(fd);
        return;
    }

    // Buffered JSON body: Content-Length, chunked, or until-close.
    std::string body_out;
    detail::ChunkedDecoder decoder;
    const std::size_t expected =
        length_header.empty()
            ? 0
            : static_cast<std::size_t>(
                  std::strtoull(length_header.c_str(), nullptr, 10));
    for (;;) {
        if (is_chunked) {
            const auto status = decoder.feed(buffer, body_out);
            if (status == detail::ChunkedDecoder::Status::Done) {
                break;
            }
            if (status == detail::ChunkedDecoder::Status::Error) {
                pal::close_fd(fd);
                emit_error(Error(ErrorCode::InternalError,
                                 "malformed chunked encoding"));
                return;
            }
        } else if (!length_header.empty()) {
            if (buffer.size() >= expected) {
                body_out = buffer.substr(0, expected);
                break;
            }
        }
        if (pal::poll_readable(fd, wake_.get(), 30000) != 1 ||
            !running_.load()) {
            pal::close_fd(fd);
            return;
        }
        char chunk[8192];
        const long n = pal::read_some(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            if (is_chunked || !length_header.empty()) {
                pal::close_fd(fd);
                emit_error(Error(ErrorCode::InternalError,
                                 "truncated HTTP response body"));
                return;
            }
            body_out = std::move(buffer);  // until-close body
            break;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
    }
    pal::close_fd(fd);
    deliver_frame(body_out);
}

void HttpClientTransport::sse_loop() {
    int failures = 0;
    while (running_.load()) {
        if (run_sse_once()) {
            failures = 0;
        } else {
            // Session-managed servers reject GETs until initialize has
            // assigned an id; don't count those warm-up attempts.
            if (session_id().empty()) {
                (void)pal::poll_readable(wake_->poll_handle(), nullptr, 200);
                continue;
            }
            ++failures;
            if (options_.max_reconnect_attempts > 0 &&
                failures >= options_.max_reconnect_attempts) {
                emit_error(Error(ErrorCode::InternalError,
                                 "SSE stream: giving up after " +
                                     std::to_string(failures) + " failures"));
                emit_close();
                return;
            }
        }
        if (!running_.load()) {
            return;
        }
        // Wait retry delay (interruptible by disconnect).
        int delay;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            delay = retry_ms_;
        }
        (void)pal::poll_readable(wake_->poll_handle(), nullptr, delay);
    }
}

bool HttpClientTransport::run_sse_once() {
    std::string error;
    pal::fd_t fd = pal::tcp_connect(options_.host, options_.port,
                                    options_.connect_timeout_ms, error);
    if (fd == pal::kInvalidFd) {
        return false;
    }

    detail::HeaderList headers{
        {"Host", options_.host + ":" + std::to_string(options_.port)},
        {"Accept", "text/event-stream"},
        {"MCP-Protocol-Version", kProtocolVersion},
        {"Cache-Control", "no-cache"},
    };
    if (options_.origin) {
        headers.emplace_back("Origin", *options_.origin);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!last_event_id_.empty()) {
            // Resume after the last delivered event (FR-TRAN-009).
            headers.emplace_back("Last-Event-ID", last_event_id_);
        }
        if (!session_id_.empty()) {
            headers.emplace_back("Mcp-Session-Id", session_id_);
        }
    }
    for (const auto& [name, value] : options_.headers) {
        headers.emplace_back(name, value);
    }
    const auto request =
        detail::serialize_request("GET", options_.path, headers, "");
    if (!pal::write_all(fd, request.data(), request.size())) {
        pal::close_fd(fd);
        return false;
    }

    std::string buffer;
    detail::HttpHead head;
    std::size_t consumed = 0;
    std::string parse_error;
    bool have_head = false;
    while (!have_head) {
        if (pal::poll_readable(fd, wake_.get(), 30000) != 1 ||
            !running_.load()) {
            pal::close_fd(fd);
            return false;
        }
        char chunk[8192];
        const long n = pal::read_some(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            pal::close_fd(fd);
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
        have_head = detail::parse_head(buffer, /*request_mode=*/false, head,
                                       consumed, parse_error);
        if (!have_head && !parse_error.empty()) {
            pal::close_fd(fd);
            return false;
        }
    }
    if (head.status != 200 ||
        !head.header_contains("content-type", "text/event-stream")) {
        pal::close_fd(fd);
        return false;
    }
    buffer.erase(0, consumed);

    bool delivered = false;
    detail::SseParser parser;
    std::vector<detail::SseEvent> events;
    parser.feed(buffer.data(), buffer.size(), events);
    for (;;) {
        for (const auto& event : events) {
            if (event.retry_ms) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                retry_ms_ = *event.retry_ms;  // server retry hint (FR-TRAN-007)
            }
            if (event.id) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_event_id_ = *event.id;
            }
            deliver_frame(event.data);
            delivered = true;
        }
        events.clear();
        if (pal::poll_readable(fd, wake_.get(), -1) != 1 ||
            !running_.load()) {
            break;  // disconnect requested
        }
        char chunk[8192];
        const long n = pal::read_some(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            break;  // server closed; caller reconnects (FR-TRAN-007)
        }
        parser.feed(chunk, static_cast<std::size_t>(n), events);
    }
    pal::close_fd(fd);
    return delivered;
}

}  // namespace mcp
