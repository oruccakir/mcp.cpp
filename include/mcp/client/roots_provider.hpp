#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <mcp/detail/json_util.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>

namespace mcp {

/// Filesystem root exposed to servers (FR-CLI-004).
struct Root {
    std::string uri;  // must be file://
    std::optional<std::string> name;
};

void to_json(json& j, const Root& r);
void from_json(const json& j, Root& r);

/// Thread-safe root registration; answers roots/list (FR-CLI-004) and feeds
/// notifications/roots/list_changed via the changed callback (FR-CLI-005).
class RootsProvider {
public:
    /// Fails unless the URI starts with file:// (FR-CLI-004) or on duplicates.
    Result<void> add_root(Root root);
    bool remove_root(const std::string& uri);
    std::vector<Root> list_roots() const;
    std::size_t size() const;

    void set_changed_callback(std::function<void()> callback);

private:
    mutable std::mutex mutex_;
    std::vector<Root> roots_;
    std::function<void()> changed_callback_;
};

}  // namespace mcp
