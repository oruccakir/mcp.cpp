#include <mcp/transport/http_server_transport.hpp>

#include <mcp/types.hpp>

#include "http/http_endpoint.hpp"
#include "http/sse_channel.hpp"

namespace mcp {

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

std::uint16_t HttpServerTransport::port() const {
    return endpoint_ ? endpoint_->port() : 0;
}

void HttpServerTransport::connect() {
    if (endpoint_) {
        return;
    }
    channel_ = std::make_unique<detail::SseChannel>(
        options_.replay_buffer_size, options_.sse_retry_ms);
    endpoint_ = std::make_unique<detail::HttpEndpoint>(
        options_, [this](const detail::HttpHead& head, const std::string& body,
                         int fd) { handle_request(head, body, fd); });
    std::string error;
    if (!endpoint_->start(error)) {
        endpoint_.reset();
        channel_.reset();
        emit_error(Error(ErrorCode::InternalError, "http listen failed: " + error));
    }
}

void HttpServerTransport::disconnect() {
    if (!endpoint_) {
        return;
    }
    channel_->shutdown();
    endpoint_->stop();

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

void HttpServerTransport::handle_request(const detail::HttpHead& head,
                                         const std::string& body, int fd) {
    if (head.method == "GET") {
        if (!head.header_contains("accept", "text/event-stream")) {
            detail::HttpEndpoint::write_simple(fd, 406, "Not Acceptable",
                                               "expected text/event-stream");
            return;
        }
        // Blocks for the lifetime of the stream (FR-TRAN-005/007).
        channel_->attach_stream(fd, head.header("last-event-id"),
                                kProtocolVersion);
        return;
    }
    if (head.method != "POST") {
        detail::HttpEndpoint::write_simple(fd, 405, "Method Not Allowed",
                                           "use POST or GET");
        return;
    }

    auto frame = parse_frame(body);
    if (!frame) {
        // 400 with a null-id JSON-RPC error body (FR-TRAN-006).
        JsonRpcResponse response;
        response.error = frame.error();
        detail::HttpEndpoint::write_simple(fd, 400, "Bad Request",
                                           serialize_message(Message(response)),
                                           "application/json");
        emit_error(frame.error());
        return;
    }

    std::function<void(Message)> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = message_handler_;
    }

    detail::PostCapture capture;
    for (const auto& item_error : frame.value().item_errors) {
        capture.add_error_response(item_error);
    }
    if (handler) {
        for (auto& message : frame.value().messages) {
            handler(std::move(message));
        }
    }

    if (capture.empty()) {
        // Only notifications/responses: 202, no body (FR-TRAN-006).
        detail::HttpEndpoint::write_simple(fd, 202, "Accepted");
        return;
    }
    detail::HttpEndpoint::write_simple(fd, 200, "OK",
                                       capture.body(frame.value().was_batch),
                                       "application/json");
}

void HttpServerTransport::send(const Message& message) {
    if (detail::PostCapture::try_capture(message)) {
        return;
    }
    if (channel_) {
        channel_->enqueue(serialize_message(message));
    }
}

void HttpServerTransport::send_batch(const std::vector<Message>& messages) {
    for (const auto& message : messages) {
        send(message);
    }
}

}  // namespace mcp
