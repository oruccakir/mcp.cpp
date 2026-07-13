#define MCP_LOG_COMPONENT "registry"

#include <mcp/server/prompt_provider.hpp>

#include <algorithm>

#include <mcp/log.hpp>

namespace mcp {

void to_json(json& j, const PromptArgument& a) {
    j = json{{"name", a.name}};
    detail::set_optional(j, "title", a.title);
    detail::set_optional(j, "description", a.description);
    detail::set_optional(j, "required", a.required);
}

void from_json(const json& j, PromptArgument& a) {
    a.name = j.at("name").get<std::string>();
    detail::get_optional(j, "title", a.title);
    detail::get_optional(j, "description", a.description);
    detail::get_optional(j, "required", a.required);
}

void to_json(json& j, const Prompt& p) {
    j = json{{"name", p.name}};
    detail::set_optional(j, "title", p.title);
    detail::set_optional(j, "description", p.description);
    detail::set_optional(j, "icons", p.icons);
    detail::set_optional(j, "arguments", p.arguments);
}

void from_json(const json& j, Prompt& p) {
    p.name = j.at("name").get<std::string>();
    detail::get_optional(j, "title", p.title);
    detail::get_optional(j, "description", p.description);
    detail::get_optional(j, "icons", p.icons);
    detail::get_optional(j, "arguments", p.arguments);
}

json GetPromptResult::to_json() const {
    json msgs = json::array();
    for (const auto& m : messages) {
        msgs.push_back(json{
            {"role", role_to_string(m.role)},
            {"content", content_to_json(m.content)},
        });
    }
    json j{{"messages", msgs}};
    detail::set_optional(j, "description", description);
    return j;
}

Result<void> PromptProvider::add_prompt(Prompt prompt, PromptHandler handler) {
    if (prompt.name.empty()) {
        return Error(ErrorCode::InvalidParams, "prompt name must not be empty");
    }
    if (!handler) {
        return Error(ErrorCode::InvalidParams,
                     "prompt '" + prompt.name + "' requires a handler");
    }
    std::function<void()> changed;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        for (const auto& entry : prompts_) {
            if (entry.first.name == prompt.name) {
                return Error(ErrorCode::InvalidParams,
                             "prompt already registered: " + prompt.name);
            }
        }
        prompts_.emplace_back(std::move(prompt), std::move(handler));
        changed = changed_callback_;
        MCP_LOG(info, "prompt registered: \""
                          << prompts_.back().first.name << "\" ("
                          << prompts_.size() << " total)");
    }
    if (changed) {
        changed();
    }
    return Result<void>::ok();
}

bool PromptProvider::remove_prompt(const std::string& name) {
    std::function<void()> changed;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        const auto it = std::find_if(
            prompts_.begin(), prompts_.end(),
            [&name](const auto& entry) { return entry.first.name == name; });
        if (it == prompts_.end()) {
            return false;
        }
        prompts_.erase(it);
        changed = changed_callback_;
    }
    if (changed) {
        changed();
    }
    return true;
}

std::size_t PromptProvider::size() const {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    return prompts_.size();
}

bool PromptProvider::has_completions() const {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    return !completions_.empty();
}

Result<detail::Page<Prompt>> PromptProvider::list_prompts(
    const std::optional<std::string>& cursor) const {
    std::vector<Prompt> snapshot;
    std::size_t page_size;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        snapshot.reserve(prompts_.size());
        for (const auto& entry : prompts_) {
            snapshot.push_back(entry.first);
        }
        page_size = page_size_;
    }
    return detail::paginate(snapshot, cursor, page_size);
}

Result<GetPromptResult> PromptProvider::get_prompt(const std::string& name,
                                                   const json& arguments) const {
    Prompt prompt;
    PromptHandler handler;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        const auto it = std::find_if(
            prompts_.begin(), prompts_.end(),
            [&name](const auto& entry) { return entry.first.name == name; });
        if (it == prompts_.end()) {
            return Error(ErrorCode::InvalidParams, "unknown prompt: " + name);
        }
        prompt = it->first;
        handler = it->second;
    }

    if (prompt.arguments) {
        for (const auto& argument : *prompt.arguments) {
            if (argument.required.value_or(false) &&
                !arguments.contains(argument.name)) {
                return Error(ErrorCode::InvalidParams,
                             "prompt '" + name +
                                 "' is missing required argument '" +
                                 argument.name + "'");
            }
        }
    }

#if defined(__cpp_exceptions)
    try {
        return handler(arguments);
    } catch (const McpError& e) {
        return e.error();
    } catch (const std::exception& e) {
        return Error(ErrorCode::InternalError, e.what());
    }
#else
    return handler(arguments);
#endif
}

void PromptProvider::set_completion(const std::string& prompt_name,
                                    const std::string& argument,
                                    CompletionCallback callback) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    completions_[{prompt_name, argument}] = std::move(callback);
}

std::optional<CompleteResult> PromptProvider::complete(
    const std::string& prompt_name, const std::string& argument,
    const std::string& value) const {
    CompletionCallback callback;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        const auto it = completions_.find({prompt_name, argument});
        if (it == completions_.end()) {
            return std::nullopt;
        }
        callback = it->second;
    }
    return callback(value);
}

void PromptProvider::set_page_size(std::size_t page_size) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    page_size_ = page_size == 0 ? 1 : page_size;
}

void PromptProvider::set_changed_callback(std::function<void()> callback) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    changed_callback_ = std::move(callback);
}

}  // namespace mcp
