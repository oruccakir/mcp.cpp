#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <mcp/transport/transport.hpp>

namespace mcp::pal {
class WakeEvent;
}

namespace mcp {

/// Configuration for the Streamable HTTP client transport (FR-TRAN-005..009).
struct HttpClientOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3001;
    std::string path = "/mcp";
    /// Extra request headers (e.g. {"Authorization", "Bearer ..."}).
    std::map<std::string, std::string> headers;
    /// Origin header value, if any.
    std::optional<std::string> origin;
    int connect_timeout_ms = 5000;
    /// Initial SSE reconnect delay; a server `retry:` hint overrides it
    /// (FR-TRAN-007).
    int reconnect_delay_ms = 1000;
    /// Consecutive SSE connect failures tolerated before giving up and
    /// firing the close handler. 0 = retry forever.
    int max_reconnect_attempts = 5;
    /// Open the GET SSE stream on connect() for server-initiated messages.
    bool open_sse_stream = true;
};

/// Client side of the MCP Streamable HTTP transport: each send() POSTs to
/// the endpoint (200 json / 202 / SSE-response bodies all handled), and a
/// background GET keeps an SSE stream open for server-initiated messages,
/// resuming with Last-Event-ID after drops (FR-TRAN-007/009).
class HttpClientTransport final : public Transport {
public:
    explicit HttpClientTransport(HttpClientOptions options = {});
    ~HttpClientTransport() override;

    void connect() override;
    void disconnect() override;

    void send(const Message& message) override;
    void send_batch(const std::vector<Message>& messages) override;

    void set_message_handler(std::function<void(Message)> handler) override;
    void set_error_handler(std::function<void(Error)> handler) override;
    void set_close_handler(std::function<void()> handler) override;

    /// Session id assigned by the server (Mcp-Session-Id), if any. Captured
    /// from response headers and echoed on subsequent requests; disconnect()
    /// sends a best-effort DELETE for it.
    std::string session_id() const;

private:
    void sse_loop();
    /// One SSE connection; returns true if it delivered any event (resets
    /// the failure counter).
    bool run_sse_once();
    void post_payload(const std::string& body);
    void send_session_delete();
    void deliver_frame(const std::string& payload);
    void emit_error(const Error& error);
    void emit_close();

    HttpClientOptions options_;
    std::atomic<bool> running_{false};
    std::atomic<bool> close_emitted_{false};
    std::unique_ptr<pal::WakeEvent> wake_;
    std::thread sse_thread_;

    std::mutex state_mutex_;
    std::string last_event_id_;
    std::string session_id_;
    int retry_ms_;

    std::mutex handler_mutex_;
    std::function<void(Message)> message_handler_;
    std::function<void(Error)> error_handler_;
    std::function<void()> close_handler_;
};

}  // namespace mcp
