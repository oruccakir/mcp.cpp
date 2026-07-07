#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <mcp/client/elicitation.hpp>
#include <mcp/client/roots_provider.hpp>
#include <mcp/client/sampling.hpp>
#include <mcp/core/session.hpp>
#include <mcp/server/logging.hpp>
#include <mcp/server/prompt_provider.hpp>
#include <mcp/server/resource_provider.hpp>
#include <mcp/server/tool_registry.hpp>

namespace mcp {

/// High-level MCP client (mirror of mcp::Server): owns the roots provider
/// and sampling/elicitation handlers, derives ClientCapabilities from what
/// is registered (so ClientSession gating stays consistent), and exposes
/// typed synchronous wrappers for the server feature surface.
///
///   mcp::Client client("my-host", "1.0.0");
///   client.set_sampling_handler(...);
///   auto init = client.connect(std::make_shared<mcp::StdioClientTransport>(params));
///   auto tools = client.list_tools();
class Client {
public:
    Client(std::string name, std::string version);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    RootsProvider& roots() { return roots_; }
    /// Declare the roots capability even while no root is registered yet.
    void enable_roots() { roots_enabled_ = true; }

    /// Configure before connect(); handlers are wired into the session at
    /// connection time and drive the declared capabilities.
    void set_sampling_handler(SamplingHandler handler);
    void set_elicitation_handler(ElicitationHandler handler);

    /// Notification hooks (configure before connect()).
    void on_tools_list_changed(std::function<void()> callback);
    void on_resources_list_changed(std::function<void()> callback);
    void on_prompts_list_changed(std::function<void()> callback);
    void on_resource_updated(std::function<void(const std::string& uri)> callback);
    void on_log_message(std::function<void(const json& params)> callback);

    /// Capabilities implied by configuration (FR-CORE-009): roots iff
    /// registered/enabled, sampling/elicitation iff a handler is set.
    ClientCapabilities capabilities() const;
    ClientOptions client_options() const;

    /// Connects the transport and performs the initialize handshake
    /// (FR-CORE-010). One connection per Client instance.
    Result<InitializeResult> connect(std::shared_ptr<Transport> transport);
    void disconnect();

    /// Underlying session for advanced use; nullptr before connect().
    ClientSession* session() { return session_.get(); }
    std::optional<ServerCapabilities> server_capabilities() const;

    void set_request_timeout(std::chrono::milliseconds timeout) {
        timeout_ = timeout;
    }

    // --- Typed wrappers over the server surface (blocking) ---

    template <typename T>
    struct ListResult {
        std::vector<T> items;
        std::optional<std::string> next_cursor;
    };

    Result<json> ping();
    Result<ListResult<Tool>> list_tools(
        std::optional<std::string> cursor = std::nullopt);
    Result<CallToolResult> call_tool(const std::string& name,
                                     json arguments = json::object());
    Result<ListResult<Resource>> list_resources(
        std::optional<std::string> cursor = std::nullopt);
    Result<ListResult<ResourceTemplate>> list_resource_templates(
        std::optional<std::string> cursor = std::nullopt);
    Result<ReadResourceResult> read_resource(const std::string& uri);
    Result<void> subscribe_resource(const std::string& uri);
    Result<void> unsubscribe_resource(const std::string& uri);
    Result<ListResult<Prompt>> list_prompts(
        std::optional<std::string> cursor = std::nullopt);
    Result<GetPromptResult> get_prompt(const std::string& name,
                                       json arguments = json::object());
    Result<CompleteResult> complete_prompt(const std::string& prompt_name,
                                           const std::string& argument,
                                           const std::string& value);
    Result<CompleteResult> complete_resource(const std::string& uri_template,
                                             const std::string& argument,
                                             const std::string& value);
    Result<void> set_logging_level(LoggingLevel level);

private:
    Result<json> request(const char* method, std::optional<json> params);
    void wire_session();

    Implementation info_;
    RootsProvider roots_;
    bool roots_enabled_ = false;
    SamplingHandler sampling_handler_;
    ElicitationHandler elicitation_handler_;

    std::function<void()> tools_changed_;
    std::function<void()> resources_changed_;
    std::function<void()> prompts_changed_;
    std::function<void(const std::string&)> resource_updated_;
    std::function<void(const json&)> log_message_;

    std::chrono::milliseconds timeout_{30000};
    std::shared_ptr<Transport> transport_;
    std::unique_ptr<ClientSession> session_;
};

}  // namespace mcp
