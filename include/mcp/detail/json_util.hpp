#pragma once

#include <optional>

#include <mcp/json.hpp>

namespace mcp::detail {

/// Sets j[key] only when the optional holds a value (MCP JSON omits absent
/// fields rather than emitting null).
template <typename T>
void set_optional(json& j, const char* key, const std::optional<T>& value) {
    if (value) {
        j[key] = *value;
    }
}

/// Reads j[key] into the optional; missing or null keys leave it empty.
template <typename T>
void get_optional(const json& j, const char* key, std::optional<T>& value) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) {
        value = it->template get<T>();
    }
}

}  // namespace mcp::detail
