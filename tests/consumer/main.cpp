// Smoke test for the installed package: exercises headers, linking, and the
// derived-capabilities path end to end.

#include <cstdio>

#include <mcp/mcp.hpp>
#include <mcp/version.hpp>

int main() {
    mcp::Server server("consumer-smoke", MCP_CPP_VERSION);

    mcp::ToolSpec echo;
    echo.description = "echo";
    echo.input_schema = mcp::json{{"type", "object"}};
    echo.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        return {{mcp::text_content(args.value("message", "hi"))}};
    };
    if (!server.register_tool("echo", std::move(echo))) {
        std::fprintf(stderr, "tool registration failed\n");
        return 1;
    }

    const mcp::json capabilities = server.capabilities();
    if (!capabilities.contains("tools")) {
        std::fprintf(stderr, "capability derivation failed\n");
        return 1;
    }

    std::printf("mcp.cpp %s consumer OK: %s\n", MCP_CPP_VERSION,
                capabilities.dump().c_str());
    return 0;
}
