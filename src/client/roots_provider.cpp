#include <mcp/client/roots_provider.hpp>

#include <algorithm>

namespace mcp {

void to_json(json& j, const Root& r) {
    j = json{{"uri", r.uri}};
    detail::set_optional(j, "name", r.name);
}

void from_json(const json& j, Root& r) {
    r.uri = j.at("uri").get<std::string>();
    detail::get_optional(j, "name", r.name);
}

Result<void> RootsProvider::add_root(Root root) {
    if (root.uri.rfind("file://", 0) != 0) {
        return Error(ErrorCode::InvalidUri,
                     "root URI must use the file:// scheme: " + root.uri);
    }
    std::function<void()> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& existing : roots_) {
            if (existing.uri == root.uri) {
                return Error(ErrorCode::InvalidParams,
                             "root already registered: " + root.uri);
            }
        }
        roots_.push_back(std::move(root));
        changed = changed_callback_;
    }
    if (changed) {
        changed();
    }
    return Result<void>::ok();
}

bool RootsProvider::remove_root(const std::string& uri) {
    std::function<void()> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(
            roots_.begin(), roots_.end(),
            [&uri](const Root& root) { return root.uri == uri; });
        if (it == roots_.end()) {
            return false;
        }
        roots_.erase(it);
        changed = changed_callback_;
    }
    if (changed) {
        changed();
    }
    return true;
}

std::vector<Root> RootsProvider::list_roots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return roots_;
}

std::size_t RootsProvider::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return roots_.size();
}

void RootsProvider::set_changed_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    changed_callback_ = std::move(callback);
}

}  // namespace mcp
