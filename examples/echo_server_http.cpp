// SRS §6.1: echo server over the Streamable HTTP transport (FR-TRAN-005..009).
//
//   ./echo_server_http --port 3001
//   curl -s -X POST localhost:3001/mcp -H 'Content-Type: application/json'
//        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}'
//   curl -N localhost:3001/mcp -H 'Accept: text/event-stream'   # SSE stream
//
// Bind+listen happens after the Server has attached its handlers, so a client
// that connects as soon as "listening on" is printed never races an unset
// handler. The server runs until killed (Streamable HTTP has no EOF like
// stdio; deploy behind a reverse proxy for TLS).

#include <future>
#include <iostream>
#include <memory>
#include <string>

#include <mcp/core/session.hpp>
#include <mcp/mcp.hpp>

int main(int argc, char** argv) {
    std::uint16_t port = 3001;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        }
    }

    mcp::Server server("echo-server-http", "1.0.0");

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

    mcp::HttpServerOptions options;
    options.host = "127.0.0.1";
    options.port = port;
    auto transport = std::make_shared<mcp::HttpServerTransport>(options);

    // Wire handlers before the accept loop starts (mirrors Server::run, but
    // lets us print the bound port before blocking).
    mcp::ServerSession session(transport, server.server_options());
    server.attach(session);
    std::promise<void> closed;
    session.set_close_callback([&closed] { closed.set_value(); });
    session.connect();  // binds + starts the accept loop
    std::cerr << "listening on http://" << options.host << ":"
              << transport->port() << options.path << std::endl;

    closed.get_future().wait();  // runs until the transport is disconnected
    return 0;
}