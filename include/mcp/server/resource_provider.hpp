#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <mcp/content.hpp>
#include <mcp/detail/pagination.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>
#include <mcp/server/completion.hpp>

namespace mcp {

/// Resource definition (FR-SRV-009).
struct Resource {
    std::string uri;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<std::vector<Icon>> icons;
    std::optional<Annotations> annotations;
};

void to_json(json& j, const Resource& r);
void from_json(const json& j, Resource& r);

/// Resource template with an RFC 6570 uriTemplate (FR-SRV-010).
struct ResourceTemplate {
    std::string uri_template;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<std::vector<Icon>> icons;
    std::optional<Annotations> annotations;
};

void to_json(json& j, const ResourceTemplate& t);
void from_json(const json& j, ResourceTemplate& t);

struct ReadResourceResult {
    std::vector<ResourceContents> contents;

    json to_json() const;
};

using ReadHandler =
    std::function<Result<ReadResourceResult>(const std::string& uri)>;

/// Thread-safe resource registration, reading, and subscriptions
/// (FR-SRV-008..013).
class ResourceProvider {
public:
    /// Fails on duplicate URIs. The optional handler serves reads for this
    /// resource; otherwise the provider-level read handler is used.
    Result<void> add_resource(Resource resource, ReadHandler handler = nullptr);
    Result<void> add_resource_template(ResourceTemplate tmpl);
    bool remove_resource(const std::string& uri);
    std::size_t resource_count() const;
    std::size_t template_count() const;
    bool has_completions() const;

    /// Fallback for reads not served by a per-resource handler, including
    /// template-matched URIs (SRS §6.3 pattern).
    void set_read_handler(ReadHandler handler);

    Result<detail::Page<Resource>> list_resources(
        const std::optional<std::string>& cursor) const;
    Result<detail::Page<ResourceTemplate>> list_resource_templates(
        const std::optional<std::string>& cursor) const;

    /// Resolution: exact resource (own handler, else fallback) -> template
    /// match (fallback) -> -32003. Structurally invalid URIs -> -32005.
    Result<ReadResourceResult> read_resource(const std::string& uri) const;

    /// Subscription bookkeeping (FR-SRV-012); the Server emits the
    /// notifications. URI must name a known resource or match a template.
    Result<void> subscribe(const std::string& uri);
    bool unsubscribe(const std::string& uri);
    bool is_subscribed(const std::string& uri) const;

    /// Registers argument completion for a template variable (FR-SRV-022).
    void set_completion(const std::string& uri_template,
                        const std::string& argument,
                        CompletionCallback callback);
    /// Returns the completion result, or nullopt if no callback matches.
    std::optional<CompleteResult> complete(const std::string& uri_template,
                                           const std::string& argument,
                                           const std::string& value) const;

    void set_page_size(std::size_t page_size);
    void set_changed_callback(std::function<void()> callback);

private:
    bool uri_known_locked(const std::string& uri) const;

    mutable std::mutex mutex_;
    std::vector<std::pair<Resource, ReadHandler>> resources_;
    std::vector<ResourceTemplate> templates_;
    ReadHandler fallback_read_;
    std::set<std::string> subscriptions_;
    std::map<std::pair<std::string, std::string>, CompletionCallback> completions_;
    std::size_t page_size_ = 100;
    std::function<void()> changed_callback_;
};

}  // namespace mcp
