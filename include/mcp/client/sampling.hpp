#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <mcp/content.hpp>
#include <mcp/core/session.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>

namespace mcp {

/// One conversation message in a sampling request (FR-CLI-001).
struct SamplingMessage {
    Role role = Role::User;
    Content content = TextContent{"", std::nullopt};
};

void to_json(json& j, const SamplingMessage& m);
void from_json(const json& j, SamplingMessage& m);

struct ModelHint {
    std::string name;
};

/// Model selection guidance (FR-CLI-003); priorities are 0.0-1.0.
struct ModelPreferences {
    std::optional<std::vector<ModelHint>> hints;
    std::optional<double> cost_priority;
    std::optional<double> speed_priority;
    std::optional<double> intelligence_priority;
};

void to_json(json& j, const ModelHint& h);
void from_json(const json& j, ModelHint& h);
void to_json(json& j, const ModelPreferences& p);
void from_json(const json& j, ModelPreferences& p);

/// Params of sampling/createMessage (FR-CLI-001).
struct CreateMessageParams {
    std::vector<SamplingMessage> messages;
    std::optional<ModelPreferences> model_preferences;
    std::optional<std::string> system_prompt;
    std::optional<std::string> include_context;  // none/thisServer/allServers
    std::optional<double> temperature;
    std::int64_t max_tokens = 1024;
    std::optional<std::vector<std::string>> stop_sequences;
    std::optional<json> metadata;
    std::optional<json> tools;        // Tool-shaped array (sampling with tools)
    std::optional<json> tool_choice;
};

void to_json(json& j, const CreateMessageParams& p);
void from_json(const json& j, CreateMessageParams& p);

/// Result of sampling/createMessage.
struct CreateMessageResult {
    Role role = Role::Assistant;
    std::vector<Content> content;
    std::string model;
    std::optional<std::string> stop_reason;  // endTurn/toolUse/maxTokens/...

    json to_json() const;
    static Result<CreateMessageResult> from_json(const json& j);
};

/// The host application's bridge to its LLM. The SDK performs no inference
/// (SRS non-goal); it delivers requests here and returns the result.
using SamplingHandler =
    std::function<Result<CreateMessageResult>(const CreateMessageParams&)>;

/// Executes one tool call during the sampling loop; the returned
/// ToolResultContent is appended to the conversation.
using SamplingToolExecutor =
    std::function<ToolResultContent(const ToolUseContent&)>;

/// Server-side multi-turn tool-use loop (FR-CLI-002): sends
/// sampling/createMessage through `session`; while the client's LLM answers
/// with tool_use content, executes each tool via `execute_tool`, appends
/// assistant(tool_use)/user(tool_result) messages, and re-requests. Stops
/// when a turn has no tool_use (e.g. endTurn) or after `max_turns`.
Result<CreateMessageResult> run_sampling_tool_loop(
    Session& session, CreateMessageParams params,
    const SamplingToolExecutor& execute_tool, int max_turns = 8,
    std::optional<std::chrono::milliseconds> per_request_timeout =
        std::chrono::milliseconds(60000));

}  // namespace mcp
