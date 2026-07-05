#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/result.hpp>
#include <mcp/types.hpp>

namespace mcp {

/// JSON-RPC 2.0 request: method call expecting a response (FR-CORE-001).
struct JsonRpcRequest {
    RequestId id;
    std::string method;
    std::optional<json> params;
};

/// JSON-RPC 2.0 notification: fire-and-forget, no id.
struct JsonRpcNotification {
    std::string method;
    std::optional<json> params;
};

/// JSON-RPC 2.0 response. Exactly one of result/error is set. A null wire id
/// (allowed for errors answering unparseable requests) maps to nullopt.
struct JsonRpcResponse {
    std::optional<RequestId> id;
    std::optional<json> result;
    std::optional<Error> error;

    bool is_error() const noexcept { return error.has_value(); }
};

using Message = std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse>;

/// Outcome of parsing one wire line/frame, which may be a single message or a
/// batch. Invalid batch elements yield per-item errors (each should be
/// answered with a null-id error response) without discarding valid ones.
struct ParsedFrame {
    std::vector<Message> messages;
    std::vector<Error> item_errors;
    bool was_batch = false;
};

json message_to_json(const Message& message);

/// Serializes to compact single-line JSON (required by stdio framing,
/// FR-TRAN-003 — embedded newlines in strings are escaped by the encoder).
std::string serialize_message(const Message& message);
std::string serialize_batch(const std::vector<Message>& messages);

/// Parses one JSON value already decoded from the wire.
Result<Message> parse_message(const json& j);

/// Parses one wire frame (single message or batch). Fails only when the
/// top-level JSON is invalid (-32700) or the batch is empty (-32600).
Result<ParsedFrame> parse_frame(std::string_view text);

}  // namespace mcp
