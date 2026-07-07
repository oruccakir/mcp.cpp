#include <mcp/client/sampling.hpp>

#include <mcp/methods.hpp>

namespace mcp {

void to_json(json& j, const SamplingMessage& m) {
    j = json{{"role", role_to_string(m.role)},
             {"content", content_to_json(m.content)}};
}

void from_json(const json& j, SamplingMessage& m) {
    const auto role = role_from_string(j.at("role").get<std::string>());
    m.role = role.value_or(Role::User);
    auto content = content_from_json(j.at("content"));
    if (content) {
        m.content = std::move(content).value();
    } else {
#if defined(__cpp_exceptions)
        throw JsonRpcError(content.error());
#endif
    }
}

void to_json(json& j, const ModelHint& h) { j = json{{"name", h.name}}; }
void from_json(const json& j, ModelHint& h) {
    h.name = j.at("name").get<std::string>();
}

void to_json(json& j, const ModelPreferences& p) {
    j = json::object();
    detail::set_optional(j, "hints", p.hints);
    detail::set_optional(j, "costPriority", p.cost_priority);
    detail::set_optional(j, "speedPriority", p.speed_priority);
    detail::set_optional(j, "intelligencePriority", p.intelligence_priority);
}

void from_json(const json& j, ModelPreferences& p) {
    detail::get_optional(j, "hints", p.hints);
    detail::get_optional(j, "costPriority", p.cost_priority);
    detail::get_optional(j, "speedPriority", p.speed_priority);
    detail::get_optional(j, "intelligencePriority", p.intelligence_priority);
}

void to_json(json& j, const CreateMessageParams& p) {
    j = json{{"messages", p.messages}, {"maxTokens", p.max_tokens}};
    detail::set_optional(j, "modelPreferences", p.model_preferences);
    detail::set_optional(j, "systemPrompt", p.system_prompt);
    detail::set_optional(j, "includeContext", p.include_context);
    detail::set_optional(j, "temperature", p.temperature);
    detail::set_optional(j, "stopSequences", p.stop_sequences);
    detail::set_optional(j, "metadata", p.metadata);
    detail::set_optional(j, "tools", p.tools);
    detail::set_optional(j, "toolChoice", p.tool_choice);
}

void from_json(const json& j, CreateMessageParams& p) {
    p.messages = j.at("messages").get<std::vector<SamplingMessage>>();
    p.max_tokens = j.value("maxTokens", std::int64_t{1024});
    detail::get_optional(j, "modelPreferences", p.model_preferences);
    detail::get_optional(j, "systemPrompt", p.system_prompt);
    detail::get_optional(j, "includeContext", p.include_context);
    detail::get_optional(j, "temperature", p.temperature);
    detail::get_optional(j, "stopSequences", p.stop_sequences);
    detail::get_optional(j, "metadata", p.metadata);
    detail::get_optional(j, "tools", p.tools);
    detail::get_optional(j, "toolChoice", p.tool_choice);
}

json CreateMessageResult::to_json() const {
    json j{{"role", role_to_string(role)},
           {"model", model}};
    // Single content object on the wire when there is exactly one block;
    // an array otherwise (sampling-with-tools shape).
    if (content.size() == 1) {
        j["content"] = content_to_json(content[0]);
    } else {
        j["content"] = content_list_to_json(content);
    }
    detail::set_optional(j, "stopReason", stop_reason);
    return j;
}

Result<CreateMessageResult> CreateMessageResult::from_json(const json& j) {
    if (!j.is_object() || !j.contains("role") || !j.contains("content") ||
        !j.contains("model")) {
        return Error(ErrorCode::InvalidParams,
                     "malformed createMessage result");
    }
    CreateMessageResult result;
    const auto role = role_from_string(j.at("role").get<std::string>());
    result.role = role.value_or(Role::Assistant);
    result.model = j.at("model").get<std::string>();
    detail::get_optional(j, "stopReason", result.stop_reason);

    const auto& content = j.at("content");
    if (content.is_array()) {
        auto list = content_list_from_json(content);
        if (!list) {
            return std::move(list.error());
        }
        result.content = std::move(list).value();
    } else {
        auto single = content_from_json(content);
        if (!single) {
            return std::move(single.error());
        }
        result.content.push_back(std::move(single).value());
    }
    return result;
}

Result<CreateMessageResult> run_sampling_tool_loop(
    Session& session, CreateMessageParams params,
    const SamplingToolExecutor& execute_tool, int max_turns,
    std::optional<std::chrono::milliseconds> per_request_timeout) {
    Session::RequestOptions options;
    options.timeout = per_request_timeout;

    for (int turn = 0; turn < max_turns; ++turn) {
        auto response = session.send_request_sync(
            methods::kSamplingCreateMessage, json(params), options);
        if (!response) {
            return std::move(response.error());
        }
        auto result = CreateMessageResult::from_json(response.value());
        if (!result) {
            return result;
        }

        std::vector<ToolUseContent> tool_uses;
        for (const auto& block : result.value().content) {
            if (const auto* use = std::get_if<ToolUseContent>(&block)) {
                tool_uses.push_back(*use);
            }
        }
        if (tool_uses.empty()) {
            return result;  // endTurn (or any non-tool stop): loop finished.
        }

        // Append the assistant turn, then one user turn with tool results.
        for (const auto& block : result.value().content) {
            params.messages.push_back(SamplingMessage{Role::Assistant, block});
        }
        for (const auto& use : tool_uses) {
            params.messages.push_back(
                SamplingMessage{Role::User, Content(execute_tool(use))});
        }
    }
    return Error(ErrorCode::InternalError,
                 "sampling tool-use loop exceeded max turns");
}

}  // namespace mcp
