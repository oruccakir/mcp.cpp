#pragma once

#include <memory>
#include <mcp/sys/threading.hpp>
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
    /// See ServerOptions::allow_reinitialize (for sessionless HTTP serving).
    void set_allow_reinitialize(bool allow) { allow_reinitialize_ = allow; }

    /// Capabilities implied by the populated registries (FR-CORE-008):
    /// tools/resources/prompts iff non-empty (listChanged, subscribe),
    /// logging always, completions iff any completion callback.
    ServerCapabilities capabilities() const;
    /// Ready-made options for constructing a ServerSession manually.
    ServerOptions server_options() const;

    /// Registers all feature handlers on the session's router and binds the
    /// logger and list_changed notifications. Multiple sessions may be
    /// attached concurrently (multi-session HTTP serving): notifications and
    /// log messages broadcast to every attached Operating session. Call
    /// detach() before destroying a session that outlives the others
    /// (run() and HttpSessionServer manage this automatically).
    void attach(ServerSession& session);
    void detach(ServerSession& session);

    /// Blocks serving the transport until the connection closes.
    /// Returns 0 on clean shutdown.
    int run(std::shared_ptr<Transport> transport);

    /// Emits notifications/resources/updated if the URI is subscribed
    /// (FR-SRV-012).
    void notify_resource_updated(const std::string& uri);

    /// The attached session when exactly one is attached (nullptr when none
    /// or several). Used to initiate server->client requests (sampling,
    /// roots, elicitation). Do not block a request handler on a round-trip
    /// to the client: dispatch is synchronous, so waiting inside a handler
    /// starves the read loop — do such work on a separate thread.
    ServerSession* session();
    /// All currently attached sessions (multi-session serving).
    std::vector<ServerSession*> sessions();

private:
    void send_notification_if_operating(const std::string& method,
                                        std::optional<json> params);

    Implementation info_;
    std::optional<std::string> instructions_;
    bool allow_reinitialize_ = false;
    ToolRegistry tools_;
    ResourceProvider resources_;
    PromptProvider prompts_;
    Logger logger_;

    mcp::sys::mutex session_mutex_;
    std::vector<ServerSession*> sessions_;
};

}  // namespace mcp
