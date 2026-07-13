#pragma once

#include <functional>
#include <mcp/sys/threading.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <mcp/content.hpp>
#include <mcp/detail/pagination.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>

namespace mcp {

/// Tool definition (FR-SRV-002).
struct Tool {
    std::string name;  // 1-128 chars, [A-Za-z0-9._-] (FR-SRV-003)
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::vector<Icon>> icons;
    json input_schema = json{{"type", "object"}};  // JSON Schema (subset validated)
    std::optional<json> output_schema;
    std::optional<Annotations> annotations;
    std::optional<json> execution;  // opaque until task support lands
};

void to_json(json& j, const Tool& tool);
void from_json(const json& j, Tool& tool);

/// Result of tools/call (FR-SRV-005). Execution failures are reported here
/// with is_error=true, not as protocol errors.
struct CallToolResult {
    std::vector<Content> content;
    bool is_error = false;

    json to_json() const;
};

/// FR-SRV-007.
using ToolHandler = std::function<CallToolResult(const json& arguments)>;

/// True if `name` satisfies FR-SRV-003.
bool is_valid_tool_name(const std::string& name);

/// Thread-safe tool registration and invocation (FR-SRV-001).
class ToolRegistry {
public:
    /// Fails on invalid (FR-SRV-003) or duplicate names.
    Result<void> register_tool(Tool tool, ToolHandler handler);
    bool remove_tool(const std::string& name);
    std::size_t size() const;

    /// Paginated listing (FR-SRV-004); invalid cursor -> -32006.
    Result<detail::Page<Tool>> list_tools(
        const std::optional<std::string>& cursor) const;

    /// Validates arguments against the tool's input schema, then invokes the
    /// handler. Unknown tool / invalid arguments are protocol errors; handler
    /// failures come back as CallToolResult{is_error=true} (FR-SRV-005).
    Result<CallToolResult> call_tool(const std::string& name,
                                     const json& arguments) const;

    void set_page_size(std::size_t page_size);
    /// Invoked after every successful register/remove (FR-SRV-006).
    void set_changed_callback(std::function<void()> callback);

private:
    mutable mcp::sys::mutex mutex_;
    std::vector<std::pair<Tool, ToolHandler>> tools_;  // insertion-ordered
    std::size_t page_size_ = 100;
    std::function<void()> changed_callback_;
};

}  // namespace mcp
