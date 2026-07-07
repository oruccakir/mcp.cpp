// SRS §6.1: minimal echo server built on the high-level Server facade.
//
// Try it:
//   ./echo_server
//   {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}
//   {"jsonrpc":"2.0","method":"notifications/initialized"}
//   {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hello"}}}

#include <memory>

#include <mcp/mcp.hpp>

int main() {
    mcp::Server server("echo-server", "1.0.0");

    mcp::ToolSpec echo;
    echo.description = "Echoes back the input";
    echo.input_schema =
        mcp::json{{"type", "object"},
                  {"properties", {{"message", {{"type", "string"}}}}},
                  {"required", mcp::json::array({"message"})}};
    echo.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        return {{mcp::text_content(args.at("message").get<std::string>())}};
    };
    server.register_tool("echo", std::move(echo));

    return server.run(std::make_shared<mcp::StdioTransport>());
}
