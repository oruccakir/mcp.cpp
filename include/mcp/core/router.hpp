#pragma once

#include <functional>
#include <mcp/sys/threading.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mcp/jsonrpc/message.hpp>

namespace mcp {

/// Dispatches incoming JSON-RPC messages to registered handlers
/// (FR-CORE-011..013). Thread-safe (FR-CONC-001); handlers are invoked
/// without internal locks held, so they may re-enter the router.
class MessageRouter {
public:
    /// Returns the result payload, or an Error that becomes a JSON-RPC error
    /// response.
    using RequestHandler = std::function<Result<json>(const std::optional<json>& params)>;
    using NotificationHandler = std::function<void(const std::optional<json>& params)>;
    /// Receives the matched response's result payload or error.
    using ResponseCallback = std::function<void(Result<json>)>;

    /// Registers a request handler. A method ending in '*' registers a
    /// wildcard prefix pattern, e.g. "experimental/*" (FR-CORE-013).
    void set_request_handler(const std::string& method, RequestHandler handler);
    void set_notification_handler(const std::string& method, NotificationHandler handler);
    bool has_request_handler(const std::string& method) const;

    /// Registers a callback awaiting the response to an outgoing request.
    void register_pending(const RequestId& id, ResponseCallback callback);
    /// Removes a pending callback without invoking it. Returns the callback
    /// if it was still registered.
    std::optional<ResponseCallback> take_pending(const RequestId& id);
    /// Fails a pending request: invokes its callback with `error`. Returns
    /// false if the id was unknown (e.g. already completed).
    bool fail_pending(const RequestId& id, const Error& error);
    /// Fails all pending requests (e.g. on transport close).
    void fail_all_pending(const Error& error);

    /// Dispatches one message. For requests, returns the response to send
    /// back (always present: unknown methods produce -32601). Notifications
    /// and responses return nullopt.
    std::optional<JsonRpcResponse> dispatch(const Message& message);

private:
    RequestHandler find_request_handler(const std::string& method) const;
    NotificationHandler find_notification_handler(const std::string& method) const;

    mutable mcp::sys::mutex mutex_;
    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notification_handlers_;
    std::vector<std::pair<std::string, RequestHandler>> wildcard_request_handlers_;
    std::vector<std::pair<std::string, NotificationHandler>> wildcard_notification_handlers_;
    std::unordered_map<RequestId, ResponseCallback> pending_;
};

}  // namespace mcp
