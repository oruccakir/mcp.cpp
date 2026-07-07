#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <mcp/json.hpp>

namespace mcp {

/// Result of completion/complete (FR-SRV-023). At most 100 values are sent
/// on the wire.
struct CompleteResult {
    std::vector<std::string> values;
    std::optional<int> total;
    std::optional<bool> has_more;

    json to_json() const;
};

/// Produces completions for the partial argument value typed so far.
using CompletionCallback =
    std::function<CompleteResult(const std::string& partial_value)>;

}  // namespace mcp
