#include <mcp/transport/http_client_transport.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <poll.h>
#include <unistd.h>

#include <mcp/error.hpp>
#include <mcp/jsonrpc/message.hpp>
#include <mcp/types.hpp>

#include "http/http_codec.hpp"
#include "http/socket_util.hpp"
#include "line_io.hpp"

namespace mcp {

namespace {

using detail::http::ChunkDecoder;
using detail::http::HttpRequest;
using detail::http::HttpResponse;
using detail::http::HttpResponseParser;
using detail::http::SseEvent;
using detail::http::SseParser;
using detail::http::header_get;
using detail::http::lower;
using detail::http::ParseStatus;
using detail::http::PollResult;
using detail::http::poll_readable;
using detail::http::serialize_request;

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

std::string host_header(const HttpClientOptions& o) {
    return o.host + ":" + std::to_string(o.port);
}

bool is_event_stream(const HttpResponse& res) {
    const auto* ct = header_get(res.headers, "content-type");
    return ct != nullptr &&
           lower(*ct).find("text/event-stream") != std::string::npos;
}

bool is_chunked(const HttpResponse& res) {
    const auto* te = header_get(res.headers, "transfer-encoding");
    return te != nullptr &&
           lower(*te).find("chunked") != std::string::npos;
}

/// Reads one chunk into `out`. Returns:
///   1 = data appended, 0 = EOF, -1 = error/timeout, -2 = wake (disconnect).
int read_chunk(int fd, int wake_fd, std::chrono::milliseconds timeout,
               std::string& out) {
    const PollResult pr =
        poll_readable(fd, wake_fd,
                      timeout.count() > 0 ? static_cast<int>(timeout.count())
                                          : -1);
    if (pr == PollResult::Wake) return -2;
    if (pr != PollResult::Ready) return -1;
    for (;;) {
        char buf[4096];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        out.append(buf, static_cast<std::size_t>(n));
        return 1;
    }
}

/// Reads response status line + headers into `parser` until Done.
bool read_head(int fd, int wake_fd, std::chrono::milliseconds timeout,
               HttpResponseParser& parser) {
    for (;;) {
        std::string chunk;
        const int rc = read_chunk(fd, wake_fd, timeout, chunk);
        if (rc == -2) return false;  // disconnect
        if (rc <= 0) return false;
        const auto st = parser.feed(chunk);
        if (st == ParseStatus::Done) return true;
        if (st == ParseStatus::Error) return false;
    }
}

}  // namespace

HttpClientTransport::HttpClientTransport() = default;

HttpClientTransport::HttpClientTransport(HttpClientOptions options)
    : options_(std::move(options)) {}

HttpClientTransport::~HttpClientTransport() { disconnect(); }

void HttpClientTransport::connect() {
    if (running_.exchange(true)) {
        return;
    }
    if (::pipe(wake_pipe_) != 0) {
        running_.store(false);
        emit_error(Error(ErrorCode::InternalError, "failed to create wake pipe"));
        return;
    }
    detail::http::set_cloexec(wake_pipe_[0], true);
    detail::http::set_cloexec(wake_pipe_[1], true);
    sse_thread_ = std::thread([this] { sse_loop(); });
}

void HttpClientTransport::disconnect() {
    const bool was = running_.exchange(false);
    if (was && wake_pipe_[1] >= 0) {
        const char byte = 0;
        (void)detail::write_all(wake_pipe_[1], &byte, 1);
    }
    if (sse_thread_.joinable() &&
        sse_thread_.get_id() != std::this_thread::get_id()) {
        sse_thread_.join();
    }
    close_fd(wake_pipe_[0]);
    close_fd(wake_pipe_[1]);
    std::function<void()> close;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        close = close_handler_;
    }
    if (was && close) {
        close();
    }
}

void HttpClientTransport::send(const Message& message) {
    post(serialize_message(message));
}

void HttpClientTransport::send_batch(const std::vector<Message>& messages) {
    post(serialize_batch(messages));
}

void HttpClientTransport::post(const std::string& body) {
    if (!running_.load()) {
        return;
    }
    int fd = detail::http::connect_tcp(options_.host, options_.port);
    if (fd < 0) {
        emit_error(Error(ErrorCode::InternalError, "HTTP connect failed"));
        return;
    }
    HttpRequest req{"POST", options_.path, {}, body};
    req.headers["host"] = host_header(options_);
    req.headers["content-type"] = "application/json";
    req.headers["accept"] = "application/json, text/event-stream";
    req.headers["content-length"] = std::to_string(body.size());
    req.headers["connection"] = "close";
    req.headers["mcp-protocol-version"] = kProtocolVersion;
    for (const auto& [k, v] : options_.headers) {
        req.headers[k] = v;
    }
    if (options_.origin) {
        req.headers["origin"] = *options_.origin;
    }
    const std::string wire = serialize_request(req);
    if (!detail::write_all(fd, wire.data(), wire.size())) {
        close_fd(fd);
        emit_error(Error(ErrorCode::InternalError, "HTTP write failed"));
        return;
    }

    HttpResponseParser parser;
    for (;;) {
        std::string chunk;
        const int rc = read_chunk(fd, wake_pipe_[0], options_.read_timeout, chunk);
        if (rc == -2) {
            close_fd(fd);
            return;  // disconnect
        }
        if (rc <= 0) {
            close_fd(fd);
            emit_error(Error(ErrorCode::InternalError, "HTTP response read failed"));
            return;
        }
        const auto st = parser.feed(chunk);
        if (st == ParseStatus::Done) break;
        if (st == ParseStatus::Error) {
            close_fd(fd);
            emit_error(Error(ErrorCode::InternalError, "HTTP response malformed"));
            return;
        }
    }

    const HttpResponse res = parser.response();
    if (res.status == 202) {
        close_fd(fd);
        return;
    }
    if (res.status < 200 || res.status >= 300) {
        close_fd(fd);
        emit_error(Error(ErrorCode::InternalError,
                         "HTTP " + std::to_string(res.status) + " " + res.reason));
        return;
    }

    if (is_event_stream(res)) {
        // Streamed POST response (FR-TRAN-007): read SSE off this connection.
        consume_sse_stream(fd, parser.leftover_body(), is_chunked(res));
    } else {
        deliver_frame(res.body);
    }
    close_fd(fd);
}

void HttpClientTransport::deliver_frame(const std::string& body) {
    if (body.empty()) {
        return;
    }
    auto frame = parse_frame(body);
    if (!frame) {
        emit_error(frame.error());
        return;
    }
    for (const auto& e : frame.value().item_errors) {
        emit_error(e);
    }
    std::function<void(Message)> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = message_handler_;
    }
    if (!handler) {
        return;
    }
    for (auto& m : frame.value().messages) {
        handler(std::move(m));
    }
}

void HttpClientTransport::consume_sse_stream(int fd, std::string leftover,
                                              bool chunked) {
    SseParser sse;
    ChunkDecoder dechunk;
    auto consume = [&](const std::string& raw) {
        const std::string bytes = chunked ? dechunk.feed(raw) : raw;
        auto events = sse.feed(bytes);
        for (const auto& ev : events) {
            if (ev.id) {
                std::lock_guard<std::mutex> lock(sse_mutex_);
                last_event_id_ = *ev.id;
            }
            if (ev.data.empty()) {
                continue;  // keepalive / initial ping
            }
            deliver_frame(ev.data);
        }
    };
    consume(leftover);
    for (;;) {
        std::string chunk;
        const int rc = read_chunk(fd, wake_pipe_[0], options_.read_timeout, chunk);
        if (rc == -2) return;  // disconnect
        if (rc <= 0) return;   // EOF/error → stream ended
        consume(chunk);
        if (chunked && dechunk.ended()) return;
    }
}

void HttpClientTransport::sse_loop() {
    while (running_.load()) {
        int fd = detail::http::connect_tcp(options_.host, options_.port);
        if (fd < 0) {
            if (!sleep_interruptible(options_.reconnect_delay_ms)) break;
            continue;
        }
        if (!send_get(fd)) {
            close_fd(fd);
            if (!sleep_interruptible(options_.reconnect_delay_ms)) break;
            continue;
        }

        HttpResponseParser parser;
        if (!read_head(fd, wake_pipe_[0], options_.read_timeout, parser)) {
            close_fd(fd);
            if (!sleep_interruptible(options_.reconnect_delay_ms)) break;
            continue;
        }
        const HttpResponse res = parser.response();
        const bool ok = res.status == 200 && is_event_stream(res);
        const std::string leftover = parser.leftover_body();
        if (!ok) {
            close_fd(fd);
            if (!sleep_interruptible(options_.reconnect_delay_ms)) break;
            continue;
        }
        consume_sse_stream(fd, leftover, is_chunked(res));
        close_fd(fd);
        if (!running_.load()) break;
        if (!sleep_interruptible(options_.reconnect_delay_ms)) break;
    }
}

bool HttpClientTransport::send_get(int fd) {
    HttpRequest req{"GET", options_.path, {}, ""};
    req.headers["host"] = host_header(options_);
    req.headers["accept"] = "text/event-stream";
    req.headers["mcp-protocol-version"] = kProtocolVersion;
    for (const auto& [k, v] : options_.headers) {
        req.headers[k] = v;
    }
    if (options_.origin) {
        req.headers["origin"] = *options_.origin;
    }
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        if (!last_event_id_.empty()) {
            req.headers["last-event-id"] = last_event_id_;
        }
    }
    const std::string wire = serialize_request(req);
    return detail::write_all(fd, wire.data(), wire.size());
}

bool HttpClientTransport::sleep_interruptible(std::chrono::milliseconds d) {
    if (d.count() <= 0) {
        // Still yield to a wake event.
        return running_.load();
    }
    const PollResult pr =
        poll_readable(-1, wake_pipe_[0], static_cast<int>(d.count()));
    if (pr == PollResult::Wake) {
        return false;  // disconnect requested
    }
    return running_.load();
}

void HttpClientTransport::set_message_handler(
    std::function<void(Message)> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    message_handler_ = std::move(handler);
}

void HttpClientTransport::set_error_handler(
    std::function<void(Error)> handler) {
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

}  // namespace mcp