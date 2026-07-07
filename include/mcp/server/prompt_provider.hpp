#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <mcp/content.hpp>
#include <mcp/detail/pagination.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>
#include <mcp/server/completion.hpp>

namespace mcp {

struct PromptArgument {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<bool> required;
};

void to_json(json& j, const PromptArgument& a);
void from_json(const json& j, PromptArgument& a);

/// Prompt definition (FR-SRV-015).
struct Prompt {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::vector<Icon>> icons;
    std::optional<std::vector<PromptArgument>> arguments;
};

void to_json(json& j, const Prompt& p);
void from_json(const json& j, Prompt& p);

enum class Role { User, Assistant };

/// Prompt message; content types per FR-SRV-016.
struct PromptMessage {
    Role role = Role::User;
    Content content = TextContent{"", std::nullopt};
};

struct GetPromptResult {
    std::optional<std::string> description;
    std::vector<PromptMessage> messages;

    json to_json() const;
};

using PromptHandler =
    std::function<Result<GetPromptResult>(const json& arguments)>;

/// Thread-safe prompt registration and retrieval (FR-SRV-014..018).
class PromptProvider {
public:
    /// Fails on duplicate names.
    Result<void> add_prompt(Prompt prompt, PromptHandler handler);
    bool remove_prompt(const std::string& name);
    std::size_t size() const;
    bool has_completions() const;

    Result<detail::Page<Prompt>> list_prompts(
        const std::optional<std::string>& cursor) const;

    /// Validates required arguments (FR-SRV-015) before invoking the handler.
    Result<GetPromptResult> get_prompt(const std::string& name,
                                       const json& arguments) const;

    /// Registers argument completion (FR-SRV-022).
    void set_completion(const std::string& prompt_name,
                        const std::string& argument,
                        CompletionCallback callback);
    std::optional<CompleteResult> complete(const std::string& prompt_name,
                                           const std::string& argument,
                                           const std::string& value) const;

    void set_page_size(std::size_t page_size);
    void set_changed_callback(std::function<void()> callback);

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<Prompt, PromptHandler>> prompts_;
    std::map<std::pair<std::string, std::string>, CompletionCallback> completions_;
    std::size_t page_size_ = 100;
    std::function<void()> changed_callback_;
};

}  // namespace mcp
