#pragma once

#include <optional>

#include <mcp/detail/json_util.hpp>
#include <mcp/json.hpp>

namespace mcp {

// Wire format uses camelCase keys per the MCP spec; C++ members are
// snake_case. All capability fields serialize with omit-if-absent semantics.

struct LoggingCapability {};

struct CompletionsCapability {};

struct PromptsCapability {
    std::optional<bool> list_changed;
};

struct ResourcesCapability {
    std::optional<bool> subscribe;
    std::optional<bool> list_changed;
};

struct ToolsCapability {
    std::optional<bool> list_changed;
};

struct TasksCapability {};

struct ExperimentalCapability {
    json value = json::object();
};

struct RootsCapability {
    std::optional<bool> list_changed;
};

struct SamplingCapability {
    std::optional<bool> tools;
    std::optional<bool> context;
};

struct ElicitationCapability {
    std::optional<bool> form;
    std::optional<bool> url;
};

/// Server capabilities per FR-CORE-008.
struct ServerCapabilities {
    std::optional<LoggingCapability> logging;
    std::optional<PromptsCapability> prompts;
    std::optional<ResourcesCapability> resources;
    std::optional<ToolsCapability> tools;
    std::optional<CompletionsCapability> completions;
    std::optional<TasksCapability> tasks;
    std::optional<ExperimentalCapability> experimental;
};

/// Client capabilities per FR-CORE-009.
struct ClientCapabilities {
    std::optional<RootsCapability> roots;
    std::optional<SamplingCapability> sampling;
    std::optional<ElicitationCapability> elicitation;
    std::optional<TasksCapability> tasks;
    std::optional<ExperimentalCapability> experimental;
};

// --- nlohmann ADL serializers -------------------------------------------

inline void to_json(json& j, const LoggingCapability&) { j = json::object(); }
inline void from_json(const json&, LoggingCapability&) {}

inline void to_json(json& j, const CompletionsCapability&) { j = json::object(); }
inline void from_json(const json&, CompletionsCapability&) {}

inline void to_json(json& j, const TasksCapability&) { j = json::object(); }
inline void from_json(const json&, TasksCapability&) {}

inline void to_json(json& j, const PromptsCapability& c) {
    j = json::object();
    detail::set_optional(j, "listChanged", c.list_changed);
}
inline void from_json(const json& j, PromptsCapability& c) {
    detail::get_optional(j, "listChanged", c.list_changed);
}

inline void to_json(json& j, const ResourcesCapability& c) {
    j = json::object();
    detail::set_optional(j, "subscribe", c.subscribe);
    detail::set_optional(j, "listChanged", c.list_changed);
}
inline void from_json(const json& j, ResourcesCapability& c) {
    detail::get_optional(j, "subscribe", c.subscribe);
    detail::get_optional(j, "listChanged", c.list_changed);
}

inline void to_json(json& j, const ToolsCapability& c) {
    j = json::object();
    detail::set_optional(j, "listChanged", c.list_changed);
}
inline void from_json(const json& j, ToolsCapability& c) {
    detail::get_optional(j, "listChanged", c.list_changed);
}

inline void to_json(json& j, const ExperimentalCapability& c) { j = c.value; }
inline void from_json(const json& j, ExperimentalCapability& c) { c.value = j; }

inline void to_json(json& j, const RootsCapability& c) {
    j = json::object();
    detail::set_optional(j, "listChanged", c.list_changed);
}
inline void from_json(const json& j, RootsCapability& c) {
    detail::get_optional(j, "listChanged", c.list_changed);
}

inline void to_json(json& j, const SamplingCapability& c) {
    j = json::object();
    detail::set_optional(j, "tools", c.tools);
    detail::set_optional(j, "context", c.context);
}
inline void from_json(const json& j, SamplingCapability& c) {
    detail::get_optional(j, "tools", c.tools);
    detail::get_optional(j, "context", c.context);
}

inline void to_json(json& j, const ElicitationCapability& c) {
    j = json::object();
    detail::set_optional(j, "form", c.form);
    detail::set_optional(j, "url", c.url);
}
inline void from_json(const json& j, ElicitationCapability& c) {
    detail::get_optional(j, "form", c.form);
    detail::get_optional(j, "url", c.url);
}

inline void to_json(json& j, const ServerCapabilities& c) {
    j = json::object();
    detail::set_optional(j, "logging", c.logging);
    detail::set_optional(j, "prompts", c.prompts);
    detail::set_optional(j, "resources", c.resources);
    detail::set_optional(j, "tools", c.tools);
    detail::set_optional(j, "completions", c.completions);
    detail::set_optional(j, "tasks", c.tasks);
    detail::set_optional(j, "experimental", c.experimental);
}
inline void from_json(const json& j, ServerCapabilities& c) {
    detail::get_optional(j, "logging", c.logging);
    detail::get_optional(j, "prompts", c.prompts);
    detail::get_optional(j, "resources", c.resources);
    detail::get_optional(j, "tools", c.tools);
    detail::get_optional(j, "completions", c.completions);
    detail::get_optional(j, "tasks", c.tasks);
    detail::get_optional(j, "experimental", c.experimental);
}

inline void to_json(json& j, const ClientCapabilities& c) {
    j = json::object();
    detail::set_optional(j, "roots", c.roots);
    detail::set_optional(j, "sampling", c.sampling);
    detail::set_optional(j, "elicitation", c.elicitation);
    detail::set_optional(j, "tasks", c.tasks);
    detail::set_optional(j, "experimental", c.experimental);
}
inline void from_json(const json& j, ClientCapabilities& c) {
    detail::get_optional(j, "roots", c.roots);
    detail::get_optional(j, "sampling", c.sampling);
    detail::get_optional(j, "elicitation", c.elicitation);
    detail::get_optional(j, "tasks", c.tasks);
    detail::get_optional(j, "experimental", c.experimental);
}

}  // namespace mcp
