#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <mcp/jsonrpc/message.hpp>
#include <mcp/transport/transport.hpp>

namespace mcp {

/// Streamable HTTP server transport options (FR-TRAN-005..009).
struct HttpServerOptions {
    /// Bind address; 127.0.0.1 by default so the server is not reachable off
    /// the local machine without an explicit opt-in (FR-TRAN-008).
    std::string host = "127.0.0.1";
    /// 0 selects an ephemeral port (read back via HttpServerTransport::port()).
    std::uint16_t port = 0;
    /// Endpoint path; client requests must target exactly this.
    std::string path = "/mcp";
    /// Allowed Origin values. Empty (default) accepts requests with no Origin
    /// header or an Origin whose host is localhost/127.0.0.1/::1. Non-empty
    /// accepts only the listed exact matches; everything else → 403
    /// (FR-TRAN-008).
    std::vector<std::string> allowed_origins;
    /// Authorization hook; returns false → 401. nullptr allows all.
    std::function<bool(const struct HttpRequestView&)> authorize;
    /// `retry:` field sent on the initial SSE event (FR-TRAN-007).
    std::chrono::milliseconds retry_ms{3000};
    /// Number of most-recent server-initiated events retained for replay on a
    /// reconnect that carries a Last-Event-ID (FR-TRAN-009).
    std::size_t replay_buffer = 256;
};

/// Read-only view of an incoming HTTP request passed to the authorize hook.
struct HttpRequestView {
    std::string method;
    std::string target;
    /// Lowercased header names → values.
    std::map<std::string, std::string> headers;
    std::string body;
};

/// Server-side Streamable HTTP transport (FR-TRAN-005..009): a single
/// endpoint accepting POST (client→server JSON-RPC) and GET (server→client
/// SSE stream). One session per transport instance; the spec lets a server
/// assign Mcp-Session-Id for many sessions, which is out of scope here.
///
/// Security: no TLS — deploy behind a reverse proxy. Binds 127.0.0.1 by
/// default. Validates Origin and runs the authorize hook before dispatch.
class HttpServerTransport final : public Transport {
public:
    HttpServerTransport();
    explicit HttpServerTransport(HttpServerOptions options);
    ~HttpServerTransport() override;

    HttpServerTransport(const HttpServerTransport&) = delete;
    HttpServerTransport& operator=(const HttpServerTransport&) = delete;

    void connect() override;
    void disconnect() override;

    void send(const Message& message) override;
    void send_batch(const std::vector<Message>& messages) override;

    void set_message_handler(std::function<void(Message)> handler) override;
    void set_error_handler(std::function<void(Error)> handler) override;
    void set_close_handler(std::function<void()> handler) override;

    /// Bound port (valid after connect()).
    std::uint16_t port() const { return port_; }

private:
    // --- connection handling ---
    void accept_loop();
    void handle_connection(int fd);
    void handle_post(int fd, HttpRequestView req);
    void handle_get(int fd, HttpRequestView req);

    // --- SSE plumbing ---
    std::uint64_t acquire_stream(int fd, std::int64_t& head_id);
    void enqueue_sse(std::string serialized);  // assigns id, buffers, notifies
    void stream_loop(int fd, std::uint64_t token, std::int64_t last_delivered);

    // --- helpers ---
    void emit_error(const Error& error);
    void emit_close();

    struct Conn;  // connection thread + done flag, defined in the .cpp

    HttpServerOptions options_;
    std::uint16_t port_ = 0;
    int listen_fd_ = -1;
    int wake_pipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    // Tracked connection threads so disconnect() can join them.
    std::mutex conns_mutex_;
    std::vector<std::unique_ptr<Conn>> conns_;

    // Handlers.
    std::mutex handler_mutex_;
    std::function<void(Message)> message_handler_;
    std::function<void(Error)> error_handler_;
    std::function<void()> close_handler_;

    // SSE state: ring buffer of recent events + the single active stream.
    struct SseEvent {
        std::int64_t id;
        std::string data;  // serialized JSON-RPC message
    };
    std::mutex sse_mutex_;
    std::condition_variable sse_cv_;
    std::int64_t next_event_id_{0};
    std::deque<SseEvent> ring_;
    std::uint64_t stream_token_{0};  // active stream's token; 0 = none yet
    int active_stream_fd_{-1};

    std::atomic<bool> close_emitted_{false};
};

}  // namespace mcp