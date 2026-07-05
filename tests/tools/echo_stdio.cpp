// Minimal MCP server over stdio used by the integration tests: supports the
// initialize handshake, ping, and a custom "echo" request.

#include <cstdio>
#include <future>
#include <memory>

#include <mcp/mcp.hpp>

int main() {
    std::fprintf(stderr, "echo-stdio started\n");

    auto transport = std::make_shared<mcp::StdioTransport>();
    mcp::ServerOptions options;
    options.server_info =
        mcp::Implementation{"echo-stdio", std::nullopt, "0.1.0"};
    options.instructions = "Test echo server";

    mcp::ServerSession session(transport, std::move(options));
    session.router().set_request_handler(
        "echo",
        [](const std::optional<mcp::json>& params) -> mcp::Result<mcp::json> {
            return mcp::json{{"echo", params.value_or(mcp::json::object())}};
        });

    std::promise<void> closed;
    session.set_close_callback([&closed] { closed.set_value(); });
    session.connect();
    closed.get_future().wait();
    return 0;
}
