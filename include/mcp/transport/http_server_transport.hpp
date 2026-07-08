#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <mcp/transport/transport.hpp>

namespace mcp::detail {
class HttpEndpoint;
class SseChannel;
struct HttpHead;
}  // namespace mcp::detail

namespace mcp {

/// Configuration for the Streamable HTTP servers (FR-TRAN-005..009).
struct HttpServerOptions {
    /// Bind address; loopback by default per FR-TRAN-008.
    std::string host = "127.0.0.1";
    /// 0 picks an ephemeral port (see port()).
    std::uint16_t port = 0;
    /// The single MCP endpoint (POST + GET).
    std::string path = "/mcp";
    /// Exact-match Origin allowlist. Localhost origins and requests without
    /// an Origin header are always accepted; anything else must be listed
    /// here or the request is answered 403 (FR-TRAN-008).
    std::vector<std::string> allowed_origins;
    /// Authentication hook: receives the (lowercased) request headers;
    /// returning false answers 401. Null allows all (FR-TRAN-008).
    std::function<bool(const std::map<std::string, std::string>& headers)> authorize;
    /// SSE retry hint sent to clients (FR-TRAN-007).
    int sse_retry_ms = 3000;
    /// Ring buffer of past SSE events kept per session for Last-Event-ID
    /// resumability (FR-TRAN-009).
    std::size_t replay_buffer_size = 256;
};

/// Sessionless server side of the MCP Streamable HTTP transport: one MCP
/// session for the whole endpoint (no Mcp-Session-Id). Suited to embedded /
/// single-client setups; use HttpSessionServer for concurrent clients.
/// POST carries client->server JSON-RPC (responses returned on the POST);
/// GET opens the SSE stream for server-initiated messages.
/// TLS is out of scope — run behind a reverse proxy for remote exposure.
class HttpServerTransport final : public Transport {
public:
    explicit HttpServerTransport(HttpServerOptions options = {});
    ~HttpServerTransport() override;

    void connect() override;
    void disconnect() override;

    void send(const Message& message) override;
    void send_batch(const std::vector<Message>& messages) override;

    void set_message_handler(std::function<void(Message)> handler) override;
    void set_error_handler(std::function<void(Error)> handler) override;
    void set_close_handler(std::function<void()> handler) override;

    /// The bound port (valid after connect(); useful with port 0).
    std::uint16_t port() const;

private:
    void handle_request(const detail::HttpHead& head, const std::string& body,
                        int fd);
    void emit_error(const Error& error);

    HttpServerOptions options_;
    std::unique_ptr<detail::HttpEndpoint> endpoint_;
    std::unique_ptr<detail::SseChannel> channel_;
    std::atomic<bool> close_emitted_{false};

    std::mutex handler_mutex_;
    std::function<void(Message)> message_handler_;
    std::function<void(Error)> error_handler_;
    std::function<void()> close_handler_;
};

}  // namespace mcp
