#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <mcp/transport/transport.hpp>

namespace mcp::detail {
struct HttpHead;
}

namespace mcp {

/// Configuration for the Streamable HTTP server transport (FR-TRAN-005..009).
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
    /// Ring buffer of past SSE events kept for Last-Event-ID resumability
    /// (FR-TRAN-009).
    std::size_t replay_buffer_size = 256;
};

/// Server side of the MCP Streamable HTTP transport: one endpoint where
/// POST carries client->server JSON-RPC (responses returned on the POST) and
/// GET opens the SSE stream for server-initiated messages. One MCP session
/// per transport instance; Mcp-Session-Id management is out of scope.
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
    std::uint16_t port() const { return bound_port_.load(); }

private:
    struct SseItem {
        std::uint64_t id;
        std::string data;
    };

    void accept_loop();
    void handle_connection(int fd);
    void serve_connection(int fd);
    void handle_post(int fd, const detail::HttpHead& head, const std::string& body);
    void handle_get(int fd, const detail::HttpHead& head);
    bool origin_allowed(const detail::HttpHead& head) const;
    bool authorized(const detail::HttpHead& head) const;
    void write_simple(int fd, int status, const std::string& reason,
                      const std::string& body = "",
                      const std::string& content_type = "text/plain");
    void enqueue_sse(const std::string& payload);
    void route_send(const Message& message);
    void emit_error(const Error& error);

    HttpServerOptions options_;
    std::atomic<bool> running_{false};
    std::atomic<bool> close_emitted_{false};
    int listen_fd_ = -1;
    int wake_pipe_[2] = {-1, -1};
    std::atomic<std::uint16_t> bound_port_{0};
    std::thread accept_thread_;

    std::mutex conn_mutex_;
    std::vector<std::thread> conn_threads_;
    std::vector<int> conn_fds_;

    std::mutex handler_mutex_;
    std::function<void(Message)> message_handler_;
    std::function<void(Error)> error_handler_;
    std::function<void()> close_handler_;

    mutable std::mutex sse_mutex_;
    std::condition_variable sse_cv_;
    std::deque<SseItem> ring_;
    std::uint64_t next_event_id_ = 1;
    /// First event id never delivered to any stream: fresh streams (no
    /// Last-Event-ID) start here so messages queued while no stream was
    /// connected are not lost, and each message goes to exactly one stream
    /// (FR-TRAN-007).
    std::uint64_t next_undelivered_ = 1;
    std::uint64_t stream_generation_ = 0;
};

}  // namespace mcp
