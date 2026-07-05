#pragma once

#include <functional>
#include <vector>

#include <mcp/error.hpp>
#include <mcp/jsonrpc/message.hpp>

namespace mcp {

/// Abstract bidirectional message transport (FR-TRAN-001). Implementations
/// deliver incoming messages on their own reader thread; handlers must be
/// installed before connect(). Custom transports (WebSocket, Unix sockets,
/// shared memory, ...) subclass this interface (FR-TRAN-010).
class Transport {
public:
    virtual ~Transport() = default;

    virtual void connect() = 0;
    virtual void disconnect() = 0;

    virtual void send(const Message& message) = 0;
    virtual void send_batch(const std::vector<Message>& messages) = 0;

    virtual void set_message_handler(std::function<void(Message)> handler) = 0;
    virtual void set_error_handler(std::function<void(Error)> handler) = 0;
    virtual void set_close_handler(std::function<void()> handler) = 0;
};

}  // namespace mcp
