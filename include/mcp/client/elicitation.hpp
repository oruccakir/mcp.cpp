#pragma once

#include <functional>
#include <optional>
#include <string>

#include <mcp/detail/json_util.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>

namespace mcp {

/// Server-initiated request for user input (FR-CLI-006): either a form
/// described by requested_schema (JSON Schema subset: text/select/boolean
/// fields) or an external URL to visit.
struct ElicitRequest {
    std::string message;
    std::optional<json> requested_schema;  // form mode
    std::optional<std::string> url;        // url mode

    static Result<ElicitRequest> from_json(const json& j) {
        if (!j.is_object() || !j.contains("message") ||
            !j.at("message").is_string()) {
            return Error(ErrorCode::InvalidParams,
                         "elicitation request requires a message");
        }
        ElicitRequest request;
        request.message = j.at("message").get<std::string>();
        detail::get_optional(j, "requestedSchema", request.requested_schema);
        detail::get_optional(j, "url", request.url);
        return request;
    }

    json to_json() const {
        json j{{"message", message}};
        detail::set_optional(j, "requestedSchema", requested_schema);
        detail::set_optional(j, "url", url);
        return j;
    }
};

enum class ElicitAction { Accept, Decline, Cancel };

inline const char* elicit_action_to_string(ElicitAction action) {
    switch (action) {
        case ElicitAction::Accept: return "accept";
        case ElicitAction::Decline: return "decline";
        case ElicitAction::Cancel: return "cancel";
    }
    return "cancel";
}

/// The user's answer. `content` carries the form values on accept.
struct ElicitResult {
    ElicitAction action = ElicitAction::Cancel;
    std::optional<json> content;

    json to_json() const {
        json j{{"action", elicit_action_to_string(action)}};
        detail::set_optional(j, "content", content);
        return j;
    }
};

/// Host-provided UI hook: present the request, return the user's answer.
using ElicitationHandler =
    std::function<Result<ElicitResult>(const ElicitRequest&)>;

}  // namespace mcp
