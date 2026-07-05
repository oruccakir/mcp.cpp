#pragma once

#include <optional>
#include <string>

#include <mcp/detail/json_util.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>
#include <mcp/types.hpp>

namespace mcp {

/// Payload of `notifications/cancelled` (FR-CORE-015).
struct CancelledNotification {
    RequestId request_id;
    std::optional<std::string> reason;

    json to_json() const {
        json j{{"requestId", request_id_to_json(request_id)}};
        detail::set_optional(j, "reason", reason);
        return j;
    }

    static Result<CancelledNotification> from_json(const json& j) {
        if (!j.is_object() || !j.contains("requestId")) {
            return Error(ErrorCode::InvalidParams, "malformed cancelled notification");
        }
        auto id = request_id_from_json(j.at("requestId"));
        if (!id) {
            return Error(ErrorCode::InvalidParams,
                         "requestId must be a string or integer");
        }
        CancelledNotification n;
        n.request_id = std::move(*id);
        detail::get_optional(j, "reason", n.reason);
        return n;
    }
};

}  // namespace mcp
