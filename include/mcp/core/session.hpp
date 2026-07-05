#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <mcp/capabilities.hpp>
#include <mcp/core/cancellation.hpp>
#include <mcp/core/progress.hpp>
#include <mcp/core/router.hpp>
#include <mcp/transport/transport.hpp>
#include <mcp/types.hpp>

namespace mcp {

/// Session lifecycle phases (FR-CORE-005).
enum class SessionState { Uninitialized, Initializing, Operating, Shutdown };

/// Stateful MCP session over a Transport: request/response correlation,
/// per-request timeouts with progress reset (FR-CORE-004/014), cancellation
/// (FR-CORE-015), and built-in ping (FR-CORE-017). Use ServerSession or
/// ClientSession for the initialize handshake and capability gating.
class Session {
public:
    struct RequestOptions {
        /// No value (or zero) means no timeout.
        std::optional<std::chrono::milliseconds> timeout;
        /// When set, injected as params._meta.progressToken so the peer can
        /// report progress; incoming progress resets the timeout clock.
        std::optional<ProgressToken> progress_token;
        std::function<void(const ProgressNotification&)> on_progress;
    };

    explicit Session(std::shared_ptr<Transport> transport);
    virtual ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Connects the underlying transport; handlers are wired at construction.
    void connect();
    void disconnect();

    MessageRouter& router() { return router_; }
    SessionState state() const { return state_.load(); }

    /// Sends a request; `callback` receives the result or error exactly once
    /// (response, timeout, cancellation, or connection close).
    RequestId send_request(const std::string& method, std::optional<json> params,
                           MessageRouter::ResponseCallback callback,
                           RequestOptions options = {});
    /// Blocking convenience wrapper around send_request.
    Result<json> send_request_sync(const std::string& method,
                                   std::optional<json> params,
                                   RequestOptions options = {});
    void send_notification(const std::string& method,
                           std::optional<json> params = std::nullopt);

    /// Cancels an outgoing request: emits notifications/cancelled, fails the
    /// callback, and ignores any late response (FR-CORE-015).
    void cancel_request(const RequestId& id,
                        std::optional<std::string> reason = std::nullopt);

    /// Emits notifications/progress for a token received from the peer.
    void send_progress(const ProgressToken& token, double progress,
                       std::optional<double> total = std::nullopt,
                       std::optional<std::string> message = std::nullopt);

    /// True if the peer cancelled this incoming request id. The initialize
    /// request is never marked cancelled (FR-CORE-015).
    bool is_cancelled(const RequestId& id) const;

    void set_close_callback(std::function<void()> callback);
    void set_error_callback(std::function<void(const Error&)> callback);

protected:
    /// Gate for incoming requests; returning an Error rejects the request
    /// without dispatching. Base implementation allows everything.
    virtual std::optional<Error> check_incoming_request(const JsonRpcRequest& request);
    /// Transport-level error hook; base forwards to the error callback.
    virtual void on_transport_error(const Error& error);

    void set_state(SessionState state) { state_.store(state); }
    void send_message(const Message& message);

    std::shared_ptr<Transport> transport_;
    MessageRouter router_;

private:
    struct Deadline {
        std::chrono::steady_clock::time_point at;
        std::chrono::milliseconds timeout{0};
        std::optional<ProgressToken> progress_token;
    };

    void handle_message(Message message);
    void handle_close();
    void handle_progress(const std::optional<json>& params);
    void handle_cancelled(const std::optional<json>& params);
    void clear_tracking(const RequestId& id);
    void timer_loop();

    std::atomic<SessionState> state_{SessionState::Uninitialized};
    std::atomic<std::int64_t> next_request_id_{1};

    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
    bool stop_timer_ = false;
    std::unordered_map<RequestId, Deadline> deadlines_;
    std::unordered_map<ProgressToken, std::function<void(const ProgressNotification&)>>
        progress_callbacks_;
    std::thread timer_thread_;

    mutable std::mutex cancelled_mutex_;
    std::unordered_set<RequestId> cancelled_incoming_;
    std::optional<RequestId> protected_request_id_;  // initialize, if seen

    std::mutex callback_mutex_;
    std::function<void()> close_callback_;
    std::function<void(const Error&)> error_callback_;
};

/// Server-side configuration.
struct ServerOptions {
    Implementation server_info;
    ServerCapabilities capabilities;
    std::optional<std::string> instructions;
    /// Preferred version first (FR-CORE-006).
    std::vector<std::string> supported_versions{kProtocolVersion};
};

/// Server end of a session: answers initialize (FR-CORE-006..010), enforces
/// initialization state (-32000) and negotiated capabilities (-32002,
/// FR-CORE-007).
class ServerSession : public Session {
public:
    ServerSession(std::shared_ptr<Transport> transport, ServerOptions options);

    std::optional<Implementation> client_info() const;
    std::optional<ClientCapabilities> client_capabilities() const;

protected:
    std::optional<Error> check_incoming_request(const JsonRpcRequest& request) override;
    void on_transport_error(const Error& error) override;

private:
    ServerOptions options_;
    mutable std::mutex peer_mutex_;
    std::optional<Implementation> client_info_;
    std::optional<ClientCapabilities> client_capabilities_;
};

/// Client-side configuration.
struct ClientOptions {
    Implementation client_info;
    ClientCapabilities capabilities;
    std::vector<std::string> supported_versions{kProtocolVersion};
    std::chrono::milliseconds initialize_timeout{10000};
};

/// Client end of a session: drives the initialize handshake (FR-CORE-010)
/// and gates incoming server requests against declared client capabilities.
class ClientSession : public Session {
public:
    ClientSession(std::shared_ptr<Transport> transport, ClientOptions options);

    /// Performs initialize → validate version → notifications/initialized.
    /// Call connect() first. Disconnects on version mismatch (FR-CORE-006).
    Result<InitializeResult> initialize();

    std::optional<Implementation> server_info() const;
    std::optional<ServerCapabilities> server_capabilities() const;

protected:
    std::optional<Error> check_incoming_request(const JsonRpcRequest& request) override;

private:
    ClientOptions options_;
    mutable std::mutex peer_mutex_;
    std::optional<Implementation> server_info_;
    std::optional<ServerCapabilities> server_capabilities_;
};

}  // namespace mcp
