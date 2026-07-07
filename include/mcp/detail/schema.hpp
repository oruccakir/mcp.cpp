#pragma once

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>

namespace mcp::detail {

/// Validates `value` against a documented subset of JSON Schema 2020-12
/// (FR-SER-003; full compliance is deferred per SRS §8):
///   - "type" (string or array of strings): object, array, string, number,
///     integer, boolean, null
///   - "properties" + "required" for objects
///   - "additionalProperties": false
///   - "items" for arrays
///   - "enum" (deep equality)
/// Unknown keywords are ignored, matching JSON Schema semantics.
Result<void> validate_schema(const json& value, const json& schema);

}  // namespace mcp::detail
