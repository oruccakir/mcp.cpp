#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <mcp/core/session.hpp>
#include <mcp/server/logging.hpp>
#include <mcp/server/prompt_provider.hpp>
#include <mcp/server/resource_provider.hpp>
#include <mcp/server/tool_registry.hpp>

namespace mcp {

/// Convenience registration shape matching the SRS §6.1 example.
struct ToolSpec {
    std::optional<std::string> title;
    std::optional<std::string> description;
    json input_schema = json{{"type", "object"}};
    std::optional<json> output_schema;
    ToolHandler handler;
};

/// High-level MCP server: owns the feature registries, derives capabilities
/// from what is registered, and wires everything into a ServerSession.
///
///   mcp::Server server("echo-server", "1.0.0");
///   server.register_tool("echo", {.description = "...", .handler = ...});
///   server.run(std::make_shared<mcp::StdioTransport>());
class Server {
public:
    Server(std::string name, std::string version);

    ToolRegistry& tools() { return tools_; }
    ResourceProvider& resources() { return resources_; }
    PromptProvider& prompts() { return prompts_; }
    Logger& logger() { return logger_; }

    Result<void> register_tool(const std::string& name, ToolSpec spec);
    void set_instructions(std::string instructions);

    /// Capabilities implied by the populated registries (FR-CORE-008):
    /// tools/resources/prompts iff non-empty (listChanged, subscribe),
    /// logging always, completions iff any completion callback.
    ServerCapabilities capabilities() const;
    /// Ready-made options for constructing a ServerSession manually.
    ServerOptions server_options() const;

    /// Registers all feature handlers on the session's router and binds the
    /// logger and list_changed notifications to it. The session must outlive
    /// the server's use of it (run() manages this automatically).
    void attach(ServerSession& session);

    /// Blocks serving the transport until the connection closes.
    /// Returns 0 on clean shutdown.
    int run(std::shared_ptr<Transport> transport);

    /// Emits notifications/resources/updated if the URI is subscribed
    /// (FR-SRV-012).
    void notify_resource_updated(const std::string& uri);

    /// The currently attached session (nullptr when not attached/serving).
    /// Used to initiate server->client requests (sampling, roots,
    /// elicitation). Do not block a request handler on a round-trip to the
    /// client: dispatch is synchronous, so waiting inside a handler starves
    /// the read loop — do such work on a separate thread.
    ServerSession* session();

private:
    void send_notification_if_operating(const std::string& method,
                                        std::optional<json> params);

    Implementation info_;
    std::optional<std::string> instructions_;
    ToolRegistry tools_;
    ResourceProvider resources_;
    PromptProvider prompts_;
    Logger logger_;

    std::mutex session_mutex_;
    ServerSession* active_session_ = nullptr;
};

}  // namespace mcp
