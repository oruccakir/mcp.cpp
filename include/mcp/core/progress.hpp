#pragma once

#include <optional>
#include <string>

#include <mcp/detail/json_util.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>
#include <mcp/types.hpp>

namespace mcp {

/// Payload of `notifications/progress` (FR-CORE-014).
struct ProgressNotification {
    ProgressToken progress_token;
    double progress = 0.0;
    std::optional<double> total;
    std::optional<std::string> message;

    json to_json() const {
        json j{{"progressToken", request_id_to_json(progress_token)},
               {"progress", progress}};
        detail::set_optional(j, "total", total);
        detail::set_optional(j, "message", message);
        return j;
    }

    static Result<ProgressNotification> from_json(const json& j) {
        if (!j.is_object() || !j.contains("progressToken") ||
            !j.contains("progress") || !j.at("progress").is_number()) {
            return Error(ErrorCode::InvalidParams, "malformed progress notification");
        }
        auto token = request_id_from_json(j.at("progressToken"));
        if (!token) {
            return Error(ErrorCode::InvalidParams,
                         "progressToken must be a string or integer");
        }
        ProgressNotification n;
        n.progress_token = std::move(*token);
        n.progress = j.at("progress").get<double>();
        detail::get_optional(j, "total", n.total);
        detail::get_optional(j, "message", n.message);
        return n;
    }
};

}  // namespace mcp
