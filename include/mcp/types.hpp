#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include <mcp/capabilities.hpp>
#include <mcp/detail/json_util.hpp>
#include <mcp/json.hpp>

namespace mcp {

/// Protocol version implemented by this SDK (pinned per SRS §3.1).
inline constexpr const char* kProtocolVersion = "2025-11-25";

/// JSON-RPC request ids and MCP progress tokens are strings or integers.
using RequestId = std::variant<std::int64_t, std::string>;
using ProgressToken = std::variant<std::int64_t, std::string>;

inline json request_id_to_json(const RequestId& id) {
    return std::visit([](const auto& v) { return json(v); }, id);
}

inline std::optional<RequestId> request_id_from_json(const json& j) {
    if (j.is_number_integer()) {
        return RequestId(j.get<std::int64_t>());
    }
    if (j.is_string()) {
        return RequestId(j.get<std::string>());
    }
    return std::nullopt;
}

inline std::string request_id_to_string(const RequestId& id) {
    return std::visit(
        [](const auto& v) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                return v;
            } else {
                return std::to_string(v);
            }
        },
        id);
}

/// Name/version pair identifying a client or server implementation.
struct Implementation {
    std::string name;
    std::optional<std::string> title;
    std::string version;
};

inline void to_json(json& j, const Implementation& impl) {
    j = json{{"name", impl.name}, {"version", impl.version}};
    detail::set_optional(j, "title", impl.title);
}
inline void from_json(const json& j, Implementation& impl) {
    impl.name = j.at("name").get<std::string>();
    impl.version = j.at("version").get<std::string>();
    detail::get_optional(j, "title", impl.title);
}

/// Params of the `initialize` request (FR-CORE-010).
struct InitializeParams {
    std::string protocol_version;
    ClientCapabilities capabilities;
    Implementation client_info;
};

inline void to_json(json& j, const InitializeParams& p) {
    j = json{{"protocolVersion", p.protocol_version},
             {"capabilities", p.capabilities},
             {"clientInfo", p.client_info}};
}
inline void from_json(const json& j, InitializeParams& p) {
    p.protocol_version = j.at("protocolVersion").get<std::string>();
    p.capabilities = j.value("capabilities", json::object()).get<ClientCapabilities>();
    p.client_info = j.at("clientInfo").get<Implementation>();
}

/// Result of the `initialize` request (FR-CORE-010).
struct InitializeResult {
    std::string protocol_version;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;
};

inline void to_json(json& j, const InitializeResult& r) {
    j = json{{"protocolVersion", r.protocol_version},
             {"capabilities", r.capabilities},
             {"serverInfo", r.server_info}};
    detail::set_optional(j, "instructions", r.instructions);
}
inline void from_json(const json& j, InitializeResult& r) {
    r.protocol_version = j.at("protocolVersion").get<std::string>();
    r.capabilities = j.value("capabilities", json::object()).get<ServerCapabilities>();
    r.server_info = j.at("serverInfo").get<Implementation>();
    detail::get_optional(j, "instructions", r.instructions);
}

}  // namespace mcp
