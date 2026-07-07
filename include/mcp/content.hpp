#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <mcp/detail/json_util.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>

namespace mcp {

/// Optional display metadata attached to content and definitions.
struct Annotations {
    std::optional<std::vector<std::string>> audience;  // "user" / "assistant"
    std::optional<double> priority;                    // 0.0 - 1.0
    std::optional<std::string> last_modified;          // ISO 8601
};

/// Icon reference usable on tools, resources, and prompts.
struct Icon {
    std::string src;
    std::optional<std::string> mime_type;
    std::optional<std::string> sizes;
};

// --- Content variants (FR-SER-002) ---------------------------------------

struct TextContent {
    std::string text;
    std::optional<Annotations> annotations;
};

struct ImageContent {
    std::string data;  // base64
    std::string mime_type;
    std::optional<Annotations> annotations;
};

struct AudioContent {
    std::string data;  // base64
    std::string mime_type;
    std::optional<Annotations> annotations;
};

/// Contents of a resource: text or base64 blob, never both.
struct ResourceContents {
    std::string uri;
    std::optional<std::string> mime_type;
    std::optional<std::string> text;
    std::optional<std::string> blob;  // base64
};

struct EmbeddedResource {
    ResourceContents resource;
    std::optional<Annotations> annotations;
};

/// Tool invocation content used by sampling (FR-CLI-002).
struct ToolUseContent {
    std::string id;
    std::string name;
    json input = json::object();
};

struct ToolResultContent;  // defined below (recursive: carries Content)

using Content =
    std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource,
                 ToolUseContent, ToolResultContent>;

/// Result of a tool invocation fed back into the sampling loop (FR-CLI-002).
struct ToolResultContent {
    std::string tool_use_id;
    std::vector<Content> content;
    bool is_error = false;
};

/// Message author role, shared by prompts (FR-SRV-016) and sampling
/// (FR-CLI-001).
enum class Role { User, Assistant };

const char* role_to_string(Role role);
std::optional<Role> role_from_string(const std::string& name);

/// Serializes with the wire "type" discriminator
/// (text/image/audio/resource/tool_use).
json content_to_json(const Content& content);
Result<Content> content_from_json(const json& j);

json content_list_to_json(const std::vector<Content>& contents);
Result<std::vector<Content>> content_list_from_json(const json& j);

// --- Convenience constructors (SRS §6 examples) ---------------------------

inline Content text_content(std::string text) {
    return TextContent{std::move(text), std::nullopt};
}
inline Content image_content(std::string base64_data, std::string mime_type) {
    return ImageContent{std::move(base64_data), std::move(mime_type),
                        std::nullopt};
}
inline Content audio_content(std::string base64_data, std::string mime_type) {
    return AudioContent{std::move(base64_data), std::move(mime_type),
                        std::nullopt};
}

// --- JSON serializers ------------------------------------------------------

void to_json(json& j, const Annotations& a);
void from_json(const json& j, Annotations& a);
void to_json(json& j, const Icon& icon);
void from_json(const json& j, Icon& icon);
void to_json(json& j, const ResourceContents& rc);
void from_json(const json& j, ResourceContents& rc);

}  // namespace mcp
