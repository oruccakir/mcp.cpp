#define MCP_LOG_COMPONENT "registry"

#include <mcp/server/tool_registry.hpp>

#include <algorithm>

#include <mcp/detail/schema.hpp>
#include <mcp/log.hpp>

namespace mcp {

void to_json(json& j, const Tool& tool) {
    j = json{{"name", tool.name}, {"inputSchema", tool.input_schema}};
    detail::set_optional(j, "title", tool.title);
    detail::set_optional(j, "description", tool.description);
    detail::set_optional(j, "icons", tool.icons);
    detail::set_optional(j, "outputSchema", tool.output_schema);
    detail::set_optional(j, "annotations", tool.annotations);
    detail::set_optional(j, "execution", tool.execution);
}

void from_json(const json& j, Tool& tool) {
    tool.name = j.at("name").get<std::string>();
    tool.input_schema = j.value("inputSchema", json{{"type", "object"}});
    detail::get_optional(j, "title", tool.title);
    detail::get_optional(j, "description", tool.description);
    detail::get_optional(j, "icons", tool.icons);
    detail::get_optional(j, "outputSchema", tool.output_schema);
    detail::get_optional(j, "annotations", tool.annotations);
    detail::get_optional(j, "execution", tool.execution);
}

json CallToolResult::to_json() const {
    return json{{"content", content_list_to_json(content)},
                {"isError", is_error}};
}

bool is_valid_tool_name(const std::string& name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](const char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

Result<void> ToolRegistry::register_tool(Tool tool, ToolHandler handler) {
    if (!is_valid_tool_name(tool.name)) {
        return Error(ErrorCode::InvalidParams,
                     "invalid tool name: '" + tool.name +
                         "' (1-128 chars from [A-Za-z0-9._-])");
    }
    if (!handler) {
        return Error(ErrorCode::InvalidParams,
                     "tool '" + tool.name + "' requires a handler");
    }
    std::function<void()> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : tools_) {
            if (entry.first.name == tool.name) {
                return Error(ErrorCode::InvalidParams,
                             "tool already registered: " + tool.name);
            }
        }
        tools_.emplace_back(std::move(tool), std::move(handler));
        changed = changed_callback_;
        MCP_LOG(info, "tool registered: \"" << tools_.back().first.name
                                            << "\" (" << tools_.size()
                                            << " total)");
    }
    if (changed) {
        changed();
    }
    return Result<void>::ok();
}

bool ToolRegistry::remove_tool(const std::string& name) {
    std::function<void()> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(
            tools_.begin(), tools_.end(),
            [&name](const auto& entry) { return entry.first.name == name; });
        if (it == tools_.end()) {
            return false;
        }
        tools_.erase(it);
        changed = changed_callback_;
    }
    MCP_LOG(info, "tool removed: \"" << name << "\"");
    if (changed) {
        changed();
    }
    return true;
}

std::size_t ToolRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.size();
}

Result<detail::Page<Tool>> ToolRegistry::list_tools(
    const std::optional<std::string>& cursor) const {
    std::vector<Tool> snapshot;
    std::size_t page_size;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(tools_.size());
        for (const auto& entry : tools_) {
            snapshot.push_back(entry.first);
        }
        page_size = page_size_;
    }
    return detail::paginate(snapshot, cursor, page_size);
}

Result<CallToolResult> ToolRegistry::call_tool(const std::string& name,
                                               const json& arguments) const {
    json input_schema;
    ToolHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(
            tools_.begin(), tools_.end(),
            [&name](const auto& entry) { return entry.first.name == name; });
        if (it == tools_.end()) {
            return Error(ErrorCode::InvalidParams, "unknown tool: " + name);
        }
        input_schema = it->first.input_schema;
        handler = it->second;
    }

    if (auto valid = detail::validate_schema(arguments, input_schema); !valid) {
        return Error(ErrorCode::InvalidParams,
                     "invalid arguments for tool '" + name +
                         "': " + valid.error().message);
    }

#if defined(__cpp_exceptions)
    try {
        return handler(arguments);
    } catch (const McpError& e) {
        // Execution failures are results, not protocol errors (FR-SRV-005).
        return CallToolResult{{text_content(e.error().message)}, true};
    } catch (const std::exception& e) {
        return CallToolResult{{text_content(e.what())}, true};
    }
#else
    return handler(arguments);
#endif
}

void ToolRegistry::set_page_size(std::size_t page_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    page_size_ = page_size == 0 ? 1 : page_size;
}

void ToolRegistry::set_changed_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    changed_callback_ = std::move(callback);
}

}  // namespace mcp
