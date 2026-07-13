#define MCP_LOG_COMPONENT "registry"

#include <mcp/server/resource_provider.hpp>

#include <algorithm>
#include <cctype>

#include <mcp/detail/uri_template.hpp>
#include <mcp/log.hpp>

namespace mcp {

namespace {

/// Minimal structural URI check (FR-SER-005/FR-SRV): "scheme:..." with an
/// RFC 3986 scheme. Full validation is a later phase.
bool looks_like_uri(const std::string& uri) {
    const auto colon = uri.find(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    if (!std::isalpha(static_cast<unsigned char>(uri[0]))) {
        return false;
    }
    for (std::size_t i = 1; i < colon; ++i) {
        const char c = uri[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' &&
            c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

}  // namespace

void to_json(json& j, const Resource& r) {
    j = json{{"uri", r.uri}, {"name", r.name}};
    detail::set_optional(j, "title", r.title);
    detail::set_optional(j, "description", r.description);
    detail::set_optional(j, "mimeType", r.mime_type);
    detail::set_optional(j, "icons", r.icons);
    detail::set_optional(j, "annotations", r.annotations);
}

void from_json(const json& j, Resource& r) {
    r.uri = j.at("uri").get<std::string>();
    r.name = j.at("name").get<std::string>();
    detail::get_optional(j, "title", r.title);
    detail::get_optional(j, "description", r.description);
    detail::get_optional(j, "mimeType", r.mime_type);
    detail::get_optional(j, "icons", r.icons);
    detail::get_optional(j, "annotations", r.annotations);
}

void to_json(json& j, const ResourceTemplate& t) {
    j = json{{"uriTemplate", t.uri_template}, {"name", t.name}};
    detail::set_optional(j, "title", t.title);
    detail::set_optional(j, "description", t.description);
    detail::set_optional(j, "mimeType", t.mime_type);
    detail::set_optional(j, "icons", t.icons);
    detail::set_optional(j, "annotations", t.annotations);
}

void from_json(const json& j, ResourceTemplate& t) {
    t.uri_template = j.at("uriTemplate").get<std::string>();
    t.name = j.at("name").get<std::string>();
    detail::get_optional(j, "title", t.title);
    detail::get_optional(j, "description", t.description);
    detail::get_optional(j, "mimeType", t.mime_type);
    detail::get_optional(j, "icons", t.icons);
    detail::get_optional(j, "annotations", t.annotations);
}

json ReadResourceResult::to_json() const {
    json arr = json::array();
    for (const auto& c : contents) {
        arr.push_back(json(c));
    }
    return json{{"contents", arr}};
}

Result<void> ResourceProvider::add_resource(Resource resource,
                                            ReadHandler handler) {
    if (!looks_like_uri(resource.uri)) {
        return Error(ErrorCode::InvalidUri, "invalid URI: " + resource.uri);
    }
    std::function<void()> changed;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        for (const auto& entry : resources_) {
            if (entry.first.uri == resource.uri) {
                return Error(ErrorCode::InvalidParams,
                             "resource already registered: " + resource.uri);
            }
        }
        resources_.emplace_back(std::move(resource), std::move(handler));
        changed = changed_callback_;
        MCP_LOG(info, "resource registered: " << resources_.back().first.uri
                                              << " (" << resources_.size()
                                              << " total)");
    }
    if (changed) {
        changed();
    }
    return Result<void>::ok();
}

Result<void> ResourceProvider::add_resource_template(ResourceTemplate tmpl) {
    std::function<void()> changed;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        for (const auto& existing : templates_) {
            if (existing.uri_template == tmpl.uri_template) {
                return Error(ErrorCode::InvalidParams,
                             "template already registered: " + tmpl.uri_template);
            }
        }
        templates_.push_back(std::move(tmpl));
        changed = changed_callback_;
        MCP_LOG(info, "resource template registered: "
                          << templates_.back().uri_template << " ("
                          << templates_.size() << " total)");
    }
    if (changed) {
        changed();
    }
    return Result<void>::ok();
}

bool ResourceProvider::remove_resource(const std::string& uri) {
    std::function<void()> changed;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        const auto it = std::find_if(
            resources_.begin(), resources_.end(),
            [&uri](const auto& entry) { return entry.first.uri == uri; });
        if (it == resources_.end()) {
            return false;
        }
        resources_.erase(it);
        subscriptions_.erase(uri);
        changed = changed_callback_;
    }
    if (changed) {
        changed();
    }
    return true;
}

std::size_t ResourceProvider::resource_count() const {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    return resources_.size();
}

std::size_t ResourceProvider::template_count() const {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    return templates_.size();
}

bool ResourceProvider::has_completions() const {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    return !completions_.empty();
}

void ResourceProvider::set_read_handler(ReadHandler handler) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    fallback_read_ = std::move(handler);
}

Result<detail::Page<Resource>> ResourceProvider::list_resources(
    const std::optional<std::string>& cursor) const {
    std::vector<Resource> snapshot;
    std::size_t page_size;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        snapshot.reserve(resources_.size());
        for (const auto& entry : resources_) {
            snapshot.push_back(entry.first);
        }
        page_size = page_size_;
    }
    return detail::paginate(snapshot, cursor, page_size);
}

Result<detail::Page<ResourceTemplate>> ResourceProvider::list_resource_templates(
    const std::optional<std::string>& cursor) const {
    std::vector<ResourceTemplate> snapshot;
    std::size_t page_size;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        snapshot = templates_;
        page_size = page_size_;
    }
    return detail::paginate(snapshot, cursor, page_size);
}

Result<ReadResourceResult> ResourceProvider::read_resource(
    const std::string& uri) const {
    if (!looks_like_uri(uri)) {
        return Error(ErrorCode::InvalidUri, "invalid URI: " + uri);
    }

    ReadHandler handler;
    bool template_matched = false;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        const auto it = std::find_if(
            resources_.begin(), resources_.end(),
            [&uri](const auto& entry) { return entry.first.uri == uri; });
        if (it != resources_.end()) {
            handler = it->second ? it->second : fallback_read_;
        } else {
            for (const auto& tmpl : templates_) {
                if (detail::match_uri_template(tmpl.uri_template, uri)) {
                    template_matched = true;
                    handler = fallback_read_;
                    break;
                }
            }
            if (!template_matched) {
                return Error(ErrorCode::ResourceNotFound,
                             "resource not found: " + uri);
            }
        }
    }

    if (!handler) {
        return Error(ErrorCode::InternalError,
                     "no read handler for resource: " + uri);
    }
#if defined(__cpp_exceptions)
    try {
        return handler(uri);
    } catch (const McpError& e) {
        return e.error();
    } catch (const std::exception& e) {
        return Error(ErrorCode::InternalError, e.what());
    }
#else
    return handler(uri);
#endif
}

Result<void> ResourceProvider::subscribe(const std::string& uri) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    if (!uri_known_locked(uri)) {
        return Error(ErrorCode::ResourceNotFound, "resource not found: " + uri);
    }
    subscriptions_.insert(uri);
    return Result<void>::ok();
}

bool ResourceProvider::unsubscribe(const std::string& uri) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    return subscriptions_.erase(uri) > 0;
}

bool ResourceProvider::is_subscribed(const std::string& uri) const {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    return subscriptions_.count(uri) > 0;
}

void ResourceProvider::set_completion(const std::string& uri_template,
                                      const std::string& argument,
                                      CompletionCallback callback) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    completions_[{uri_template, argument}] = std::move(callback);
}

std::optional<CompleteResult> ResourceProvider::complete(
    const std::string& uri_template, const std::string& argument,
    const std::string& value) const {
    CompletionCallback callback;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        const auto it = completions_.find({uri_template, argument});
        if (it == completions_.end()) {
            return std::nullopt;
        }
        callback = it->second;
    }
    return callback(value);
}

void ResourceProvider::set_page_size(std::size_t page_size) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    page_size_ = page_size == 0 ? 1 : page_size;
}

void ResourceProvider::set_changed_callback(std::function<void()> callback) {
    std::lock_guard<mcp::sys::mutex> lock(mutex_);
    changed_callback_ = std::move(callback);
}

bool ResourceProvider::uri_known_locked(const std::string& uri) const {
    for (const auto& entry : resources_) {
        if (entry.first.uri == uri) {
            return true;
        }
    }
    for (const auto& tmpl : templates_) {
        if (detail::match_uri_template(tmpl.uri_template, uri)) {
            return true;
        }
    }
    return false;
}

}  // namespace mcp
