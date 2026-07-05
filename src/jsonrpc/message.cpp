#include <mcp/jsonrpc/message.hpp>

namespace mcp {

namespace {

json request_to_json(const JsonRpcRequest& r) {
    json j{{"jsonrpc", "2.0"},
           {"id", request_id_to_json(r.id)},
           {"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
    return j;
}

json notification_to_json(const JsonRpcNotification& n) {
    json j{{"jsonrpc", "2.0"}, {"method", n.method}};
    if (n.params) {
        j["params"] = *n.params;
    }
    return j;
}

json response_to_json(const JsonRpcResponse& r) {
    json j{{"jsonrpc", "2.0"}};
    j["id"] = r.id ? request_id_to_json(*r.id) : json(nullptr);
    if (r.error) {
        j["error"] = r.error->to_json();
    } else {
        j["result"] = r.result.value_or(json(nullptr));
    }
    return j;
}

bool valid_params(const json& j) { return j.is_object() || j.is_array(); }

}  // namespace

json message_to_json(const Message& message) {
    return std::visit(
        [](const auto& m) -> json {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, JsonRpcRequest>) {
                return request_to_json(m);
            } else if constexpr (std::is_same_v<T, JsonRpcNotification>) {
                return notification_to_json(m);
            } else {
                return response_to_json(m);
            }
        },
        message);
}

std::string serialize_message(const Message& message) {
    return message_to_json(message).dump();
}

std::string serialize_batch(const std::vector<Message>& messages) {
    json arr = json::array();
    for (const auto& m : messages) {
        arr.push_back(message_to_json(m));
    }
    return arr.dump();
}

Result<Message> parse_message(const json& j) {
    if (!j.is_object()) {
        return Error(ErrorCode::InvalidRequest, "message is not a JSON object");
    }
    const auto ver_it = j.find("jsonrpc");
    if (ver_it == j.end() || !ver_it->is_string() ||
        ver_it->get<std::string>() != "2.0") {
        return Error(ErrorCode::InvalidRequest, "missing or invalid jsonrpc version");
    }

    const auto method_it = j.find("method");
    const auto id_it = j.find("id");

    if (method_it != j.end()) {
        if (!method_it->is_string()) {
            return Error(ErrorCode::InvalidRequest, "method must be a string");
        }
        std::optional<json> params;
        if (auto it = j.find("params"); it != j.end() && !it->is_null()) {
            if (!valid_params(*it)) {
                return Error(ErrorCode::InvalidRequest,
                             "params must be an object or array");
            }
            params = *it;
        }
        if (id_it == j.end()) {
            return Message(JsonRpcNotification{method_it->get<std::string>(),
                                               std::move(params)});
        }
        auto id = request_id_from_json(*id_it);
        if (!id) {
            return Error(ErrorCode::InvalidRequest,
                         "request id must be a string or integer");
        }
        return Message(JsonRpcRequest{std::move(*id), method_it->get<std::string>(),
                                      std::move(params)});
    }

    // No method: must be a response.
    const bool has_result = j.contains("result");
    const auto error_it = j.find("error");
    const bool has_error = error_it != j.end();
    if (has_result == has_error) {
        return Error(ErrorCode::InvalidRequest,
                     "response must contain exactly one of result/error");
    }
    if (id_it == j.end()) {
        return Error(ErrorCode::InvalidRequest, "response is missing id");
    }

    JsonRpcResponse resp;
    if (!id_it->is_null()) {
        auto id = request_id_from_json(*id_it);
        if (!id) {
            return Error(ErrorCode::InvalidRequest,
                         "response id must be a string, integer, or null");
        }
        resp.id = std::move(*id);
    }
    if (has_error) {
        if (!error_it->is_object() || !error_it->contains("code") ||
            !error_it->contains("message")) {
            return Error(ErrorCode::InvalidRequest, "malformed error object");
        }
        resp.error = Error::from_json(*error_it);
    } else {
        resp.result = j.at("result");
    }
    return Message(std::move(resp));
}

Result<ParsedFrame> parse_frame(std::string_view text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        return Error(ErrorCode::ParseError, "invalid JSON");
    }

    ParsedFrame frame;
    if (j.is_array()) {
        frame.was_batch = true;
        if (j.empty()) {
            return Error(ErrorCode::InvalidRequest, "batch must not be empty");
        }
        for (const auto& item : j) {
            auto parsed = parse_message(item);
            if (parsed) {
                frame.messages.push_back(std::move(parsed).value());
            } else {
                frame.item_errors.push_back(std::move(parsed.error()));
            }
        }
        return frame;
    }

    auto parsed = parse_message(j);
    if (!parsed) {
        return std::move(parsed.error());
    }
    frame.messages.push_back(std::move(parsed).value());
    return frame;
}

}  // namespace mcp
