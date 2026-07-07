#include <mcp/content.hpp>

namespace mcp {

void to_json(json& j, const Annotations& a) {
    j = json::object();
    detail::set_optional(j, "audience", a.audience);
    detail::set_optional(j, "priority", a.priority);
    detail::set_optional(j, "lastModified", a.last_modified);
}
void from_json(const json& j, Annotations& a) {
    detail::get_optional(j, "audience", a.audience);
    detail::get_optional(j, "priority", a.priority);
    detail::get_optional(j, "lastModified", a.last_modified);
}

void to_json(json& j, const Icon& icon) {
    j = json{{"src", icon.src}};
    detail::set_optional(j, "mimeType", icon.mime_type);
    detail::set_optional(j, "sizes", icon.sizes);
}
void from_json(const json& j, Icon& icon) {
    icon.src = j.at("src").get<std::string>();
    detail::get_optional(j, "mimeType", icon.mime_type);
    detail::get_optional(j, "sizes", icon.sizes);
}

void to_json(json& j, const ResourceContents& rc) {
    j = json{{"uri", rc.uri}};
    detail::set_optional(j, "mimeType", rc.mime_type);
    detail::set_optional(j, "text", rc.text);
    detail::set_optional(j, "blob", rc.blob);
}
void from_json(const json& j, ResourceContents& rc) {
    rc.uri = j.at("uri").get<std::string>();
    detail::get_optional(j, "mimeType", rc.mime_type);
    detail::get_optional(j, "text", rc.text);
    detail::get_optional(j, "blob", rc.blob);
}

namespace {

void set_annotations(json& j, const std::optional<Annotations>& annotations) {
    detail::set_optional(j, "annotations", annotations);
}

std::optional<Annotations> get_annotations(const json& j) {
    std::optional<Annotations> annotations;
    detail::get_optional(j, "annotations", annotations);
    return annotations;
}

}  // namespace

json content_to_json(const Content& content) {
    return std::visit(
        [](const auto& c) -> json {
            using T = std::decay_t<decltype(c)>;
            json j;
            if constexpr (std::is_same_v<T, TextContent>) {
                j = json{{"type", "text"}, {"text", c.text}};
                set_annotations(j, c.annotations);
            } else if constexpr (std::is_same_v<T, ImageContent>) {
                j = json{{"type", "image"},
                         {"data", c.data},
                         {"mimeType", c.mime_type}};
                set_annotations(j, c.annotations);
            } else if constexpr (std::is_same_v<T, AudioContent>) {
                j = json{{"type", "audio"},
                         {"data", c.data},
                         {"mimeType", c.mime_type}};
                set_annotations(j, c.annotations);
            } else if constexpr (std::is_same_v<T, EmbeddedResource>) {
                j = json{{"type", "resource"}, {"resource", c.resource}};
                set_annotations(j, c.annotations);
            } else {
                j = json{{"type", "tool_use"},
                         {"id", c.id},
                         {"name", c.name},
                         {"input", c.input}};
            }
            return j;
        },
        content);
}

Result<Content> content_from_json(const json& j) {
    if (!j.is_object() || !j.contains("type") || !j.at("type").is_string()) {
        return Error(ErrorCode::InvalidParams, "content is missing a type");
    }
    const auto type = j.at("type").get<std::string>();
#if defined(__cpp_exceptions)
    try {
#endif
        if (type == "text") {
            return Content(TextContent{j.at("text").get<std::string>(),
                                       get_annotations(j)});
        }
        if (type == "image") {
            return Content(ImageContent{j.at("data").get<std::string>(),
                                        j.at("mimeType").get<std::string>(),
                                        get_annotations(j)});
        }
        if (type == "audio") {
            return Content(AudioContent{j.at("data").get<std::string>(),
                                        j.at("mimeType").get<std::string>(),
                                        get_annotations(j)});
        }
        if (type == "resource") {
            return Content(EmbeddedResource{
                j.at("resource").get<ResourceContents>(), get_annotations(j)});
        }
        if (type == "tool_use") {
            return Content(ToolUseContent{j.at("id").get<std::string>(),
                                          j.at("name").get<std::string>(),
                                          j.value("input", json::object())});
        }
#if defined(__cpp_exceptions)
    } catch (const json::exception& e) {
        return Error(ErrorCode::InvalidParams,
                     std::string("malformed content: ") + e.what());
    }
#endif
    return Error(ErrorCode::InvalidParams, "unknown content type: " + type);
}

json content_list_to_json(const std::vector<Content>& contents) {
    json arr = json::array();
    for (const auto& c : contents) {
        arr.push_back(content_to_json(c));
    }
    return arr;
}

Result<std::vector<Content>> content_list_from_json(const json& j) {
    if (!j.is_array()) {
        return Error(ErrorCode::InvalidParams, "content must be an array");
    }
    std::vector<Content> contents;
    contents.reserve(j.size());
    for (const auto& item : j) {
        auto parsed = content_from_json(item);
        if (!parsed) {
            return std::move(parsed.error());
        }
        contents.push_back(std::move(parsed).value());
    }
    return contents;
}

}  // namespace mcp
