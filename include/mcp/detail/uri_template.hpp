#pragma once

#include <map>
#include <optional>
#include <string>

namespace mcp::detail {

/// RFC 6570 level-1 matching (FR-SRV-010 subset): literal segments plus
/// simple `{var}` expressions. A variable matches one or more characters up
/// to the next literal; a trailing variable matches the rest of the URI
/// (including '/', so `file:///{path}` covers nested paths).
/// Returns the extracted variables, or nullopt if the URI does not match.
std::optional<std::map<std::string, std::string>> match_uri_template(
    const std::string& uri_template, const std::string& uri);

}  // namespace mcp::detail
