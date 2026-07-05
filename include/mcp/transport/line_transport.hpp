#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <mcp/transport/transport.hpp>

namespace mcp {

/// Shared base for newline-delimited JSON transports (FR-TRAN-003):
/// serializes outgoing messages to single-line JSON and parses incoming
/// lines, fanning out to the registered handlers.
class LineTransportBase : public Transport {
public:
    void send(const Message& message) override;
    void send_batch(const std::vector<Message>& messages) override;

    void set_message_handler(std::function<void(Message)> handler) override;
    void set_error_handler(std::function<void(Error)> handler) override;
    void set_close_handler(std::function<void()> handler) override;

protected:
    /// Writes one framed line (without trailing newline; implementations
    /// append it). Called with the write lock held.
    virtual void write_line(const std::string& line) = 0;

    /// Parses one received line and dispatches to handlers. Per-item batch
    /// errors and unparseable frames go to the error handler.
    void process_line(const std::string& line);

    void emit_error(const Error& error);
    void emit_close();

private:
    void send_line(const std::string& line);

    std::mutex write_mutex_;
    std::mutex handler_mutex_;
    std::function<void(Message)> message_handler_;
    std::function<void(Error)> error_handler_;
    std::function<void()> close_handler_;
};

}  // namespace mcp
