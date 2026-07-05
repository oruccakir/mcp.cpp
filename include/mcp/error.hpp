#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <mcp/detail/json_util.hpp>
#include <mcp/json.hpp>

namespace mcp {

/// JSON-RPC 2.0 (FR-CORE-002) and MCP-specific (FR-CORE-003) error codes.
enum class ErrorCode : int {
    // JSON-RPC 2.0
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,

    // MCP-specific (within the JSON-RPC server-error range)
    ConnectionNotInitialized = -32000,
    RequestAlreadyInProgress = -32001,
    CapabilityNotSupported = -32002,
    ResourceNotFound = -32003,
    ToolExecutionFailed = -32004,
    InvalidUri = -32005,
    PaginationError = -32006,
};

/// A JSON-RPC error object: code, message, optional structured data.
struct Error {
    int code = static_cast<int>(ErrorCode::InternalError);
    std::string message;
    std::optional<json> data;

    Error() = default;
    Error(int code_, std::string message_, std::optional<json> data_ = std::nullopt)
        : code(code_), message(std::move(message_)), data(std::move(data_)) {}
    Error(ErrorCode code_, std::string message_, std::optional<json> data_ = std::nullopt)
        : Error(static_cast<int>(code_), std::move(message_), std::move(data_)) {}

    json to_json() const {
        json j{{"code", code}, {"message", message}};
        detail::set_optional(j, "data", data);
        return j;
    }

    static Error from_json(const json& j) {
        Error e;
        e.code = j.at("code").get<int>();
        e.message = j.at("message").get<std::string>();
        detail::get_optional(j, "data", e.data);
        return e;
    }
};

/// Root of the SDK exception hierarchy (FR-ERR-002).
class McpError : public std::runtime_error {
public:
    explicit McpError(Error error)
        : std::runtime_error(error.message), error_(std::move(error)) {}

    const Error& error() const noexcept { return error_; }
    int code() const noexcept { return error_.code; }

private:
    Error error_;
};

class JsonRpcError : public McpError {
public:
    using McpError::McpError;
};

class TransportError : public McpError {
public:
    using McpError::McpError;
    explicit TransportError(std::string message)
        : McpError(Error(ErrorCode::InternalError, std::move(message))) {}
};

class CapabilityError : public McpError {
public:
    explicit CapabilityError(std::string message)
        : McpError(Error(ErrorCode::CapabilityNotSupported, std::move(message))) {}
};

class SessionError : public McpError {
public:
    using McpError::McpError;
};

}  // namespace mcp
