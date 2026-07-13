#include <mcp/transport/line_transport.hpp>

namespace mcp {

void LineTransportBase::send(const Message& message) {
    send_line(serialize_message(message));
}

void LineTransportBase::send_batch(const std::vector<Message>& messages) {
    send_line(serialize_batch(messages));
}

void LineTransportBase::send_line(const std::string& line) {
    // serialize_* produces compact JSON with control characters escaped, so
    // this only trips on a misuse of write paths (FR-TRAN-003).
    if (line.find('\n') != std::string::npos) {
        emit_error(Error(ErrorCode::InternalError,
                         "refusing to send message containing raw newline"));
        return;
    }
    std::lock_guard<mcp::sys::mutex> lock(write_mutex_);
    write_line(line);
}

void LineTransportBase::set_message_handler(std::function<void(Message)> handler) {
    std::lock_guard<mcp::sys::mutex> lock(handler_mutex_);
    message_handler_ = std::move(handler);
}

void LineTransportBase::set_error_handler(std::function<void(Error)> handler) {
    std::lock_guard<mcp::sys::mutex> lock(handler_mutex_);
    error_handler_ = std::move(handler);
}

void LineTransportBase::set_close_handler(std::function<void()> handler) {
    std::lock_guard<mcp::sys::mutex> lock(handler_mutex_);
    close_handler_ = std::move(handler);
}

void LineTransportBase::process_line(const std::string& line) {
    if (line.empty()) {
        return;
    }
    auto frame = parse_frame(line);
    if (!frame) {
        emit_error(frame.error());
        return;
    }
    for (const auto& item_error : frame.value().item_errors) {
        emit_error(item_error);
    }

    std::function<void(Message)> handler;
    {
        std::lock_guard<mcp::sys::mutex> lock(handler_mutex_);
        handler = message_handler_;
    }
    if (!handler) {
        return;
    }
    for (auto& message : frame.value().messages) {
        handler(std::move(message));
    }
}

void LineTransportBase::emit_error(const Error& error) {
    std::function<void(Error)> handler;
    {
        std::lock_guard<mcp::sys::mutex> lock(handler_mutex_);
        handler = error_handler_;
    }
    if (handler) {
        handler(error);
    }
}

void LineTransportBase::emit_close() {
    std::function<void()> handler;
    {
        std::lock_guard<mcp::sys::mutex> lock(handler_mutex_);
        handler = close_handler_;
    }
    if (handler) {
        handler();
    }
}

}  // namespace mcp
