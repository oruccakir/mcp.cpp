#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <mcp/jsonrpc/message.hpp>
#include <mcp/transport/transport.hpp>

namespace mcp {

/// Streamable HTTP client transport options (FR-TRAN-005..009).
struct HttpClientOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3001;
    std::string path = "/mcp";
    /// Extra request headers (e.g. Authorization). Lowercased keys preferred.
    std::map<std::string, std::string> headers;
    /// Origin header value, sent on every request if non-empty.
    std::optional<std::string> origin;
    /// Timeout for establishing a TCP connection (POST and SSE reconnect).
    std::chrono::milliseconds connect_timeout{5000};
    /// Read idle timeout on the SSE stream before treating it as stale and
    /// reconnecting. The server sends its own retry hint, which overrides this.
    std::chrono::milliseconds read_timeout{30000};
    /// Initial reconnect delay; doubled by the server's retry hint when given.
    std::chrono::milliseconds reconnect_delay_ms{1000};
};

/// Client-side Streamable HTTP transport (FR-TRAN-005..009): POSTs JSON-RPC
/// to the endpoint (one request per connection, Connection: close) and
/// maintains a GET SSE stream for server-initiated messages, reconnecting with
/// Last-Event-ID on drop (FR-TRAN-007/009).
///
/// A POST response may itself be an SSE stream (another server streaming the
/// reply); that is read inline until the stream ends. JSON responses are
/// parsed and each JSON-RPC message is delivered to the message handler.
class HttpClientTransport final : public Transport {
public:
    HttpClientTransport();
    explicit HttpClientTransport(HttpClientOptions options);
    ~HttpClientTransport() override;

    HttpClientTransport(const HttpClientTransport&) = delete;
    HttpClientTransport& operator=(const HttpClientTransport&) = delete;

    void connect() override;
    void disconnect() override;

    void send(const Message& message) override;
    void send_batch(const std::vector<Message>& messages) override;

    void set_message_handler(std::function<void(Message)> handler) override;
    void set_error_handler(std::function<void(Error)> handler) override;
    void set_close_handler(std::function<void()> handler) override;

private:
    void post(const std::string& body);     // one POST per call (FR-TRAN-006)
    void deliver_frame(const std::string& body);  // parse_frame → message handler
    void sse_loop();                         // GET stream + reconnect loop
    bool send_get(int fd);                   // sends the GET request
    void consume_sse_stream(int fd, std::string leftover, bool chunked);
    bool sleep_interruptible(std::chrono::milliseconds d);

    void emit_error(const Error& error);

    HttpClientOptions options_;
    int wake_pipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::thread sse_thread_;

    std::mutex handler_mutex_;
    std::function<void(Message)> message_handler_;
    std::function<void(Error)> error_handler_;
    std::function<void()> close_handler_;

    // SSE state shared between send()-driven POST reads and the SSE thread.
    std::mutex sse_mutex_;
    std::string last_event_id_;  // Last-Event-ID for reconnect (FR-TRAN-009)
};

}  // namespace mcp