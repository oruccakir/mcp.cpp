#include <mcp/core/router.hpp>

#include <algorithm>

namespace mcp {

namespace {

bool is_wildcard(const std::string& method) {
    return !method.empty() && method.back() == '*';
}

template <typename Handler>
Handler match_wildcard(const std::vector<std::pair<std::string, Handler>>& patterns,
                       const std::string& method) {
    // Longest matching prefix wins.
    const std::pair<std::string, Handler>* best = nullptr;
    for (const auto& entry : patterns) {
        if (method.compare(0, entry.first.size(), entry.first) == 0 &&
            (!best || entry.first.size() > best->first.size())) {
            best = &entry;
        }
    }
    return best ? best->second : Handler();
}

}  // namespace

void MessageRouter::set_request_handler(const std::string& method,
                                        RequestHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_wildcard(method)) {
        const std::string prefix = method.substr(0, method.size() - 1);
        wildcard_request_handlers_.emplace_back(prefix, std::move(handler));
    } else {
        request_handlers_[method] = std::move(handler);
    }
}

void MessageRouter::set_notification_handler(const std::string& method,
                                             NotificationHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_wildcard(method)) {
        const std::string prefix = method.substr(0, method.size() - 1);
        wildcard_notification_handlers_.emplace_back(prefix, std::move(handler));
    } else {
        notification_handlers_[method] = std::move(handler);
    }
}

bool MessageRouter::has_request_handler(const std::string& method) const {
    return static_cast<bool>(find_request_handler(method));
}

MessageRouter::RequestHandler MessageRouter::find_request_handler(
    const std::string& method) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = request_handlers_.find(method); it != request_handlers_.end()) {
        return it->second;
    }
    return match_wildcard(wildcard_request_handlers_, method);
}

MessageRouter::NotificationHandler MessageRouter::find_notification_handler(
    const std::string& method) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = notification_handlers_.find(method);
        it != notification_handlers_.end()) {
        return it->second;
    }
    return match_wildcard(wildcard_notification_handlers_, method);
}

void MessageRouter::register_pending(const RequestId& id, ResponseCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_[id] = std::move(callback);
}

std::optional<MessageRouter::ResponseCallback> MessageRouter::take_pending(
    const RequestId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(id);
    if (it == pending_.end()) {
        return std::nullopt;
    }
    auto callback = std::move(it->second);
    pending_.erase(it);
    return callback;
}

bool MessageRouter::fail_pending(const RequestId& id, const Error& error) {
    auto callback = take_pending(id);
    if (!callback) {
        return false;
    }
    (*callback)(Result<json>::err(error));
    return true;
}

void MessageRouter::fail_all_pending(const Error& error) {
    std::unordered_map<RequestId, ResponseCallback> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(pending_);
    }
    for (auto& [id, callback] : pending) {
        callback(Result<json>::err(error));
    }
}

std::optional<JsonRpcResponse> MessageRouter::dispatch(const Message& message) {
    if (const auto* request = std::get_if<JsonRpcRequest>(&message)) {
        JsonRpcResponse response;
        response.id = request->id;

        auto handler = find_request_handler(request->method);
        if (!handler) {
            response.error = Error(ErrorCode::MethodNotFound,
                                   "Method not found: " + request->method);
            return response;
        }
#if defined(__cpp_exceptions)
        try {
#endif
            auto result = handler(request->params);
            if (result) {
                response.result = std::move(result).value();
            } else {
                response.error = std::move(result.error());
            }
#if defined(__cpp_exceptions)
        } catch (const McpError& e) {
            response.error = e.error();
        } catch (const std::exception& e) {
            response.error = Error(ErrorCode::InternalError, e.what());
        }
#endif
        return response;
    }

    if (const auto* notification = std::get_if<JsonRpcNotification>(&message)) {
        if (auto handler = find_notification_handler(notification->method)) {
            handler(notification->params);
        }
        return std::nullopt;
    }

    const auto& response = std::get<JsonRpcResponse>(message);
    if (!response.id) {
        return std::nullopt;  // Null-id responses cannot be matched.
    }
    if (auto callback = take_pending(*response.id)) {
        if (response.error) {
            (*callback)(Result<json>::err(*response.error));
        } else {
            (*callback)(Result<json>::ok(response.result.value_or(json(nullptr))));
        }
    }
    return std::nullopt;
}

}  // namespace mcp
