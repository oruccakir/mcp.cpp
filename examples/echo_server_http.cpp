// Echo server over the multi-session Streamable HTTP transport
// (FR-TRAN-005..009 + Mcp-Session-Id management).
//
//   ./echo_server_http --port 3001
//   Each client initialize opens its own session; the id comes back in the
//   Mcp-Session-Id response header and must be echoed on later requests.
//
// TLS is out of scope; bind stays on 127.0.0.1 unless fronted by a proxy.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <unistd.h>

#include <mcp/mcp.hpp>

int main(int argc, char** argv) {
    mcp::HttpSessionServerOptions options;
    options.http.port = 3001;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) {
            options.http.port =
                static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
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

    mcp::HttpSessionServer http(
        options,
        [&server](std::shared_ptr<mcp::Transport> transport) {
            auto session = std::make_unique<mcp::ServerSession>(
                std::move(transport), server.server_options());
            server.attach(*session);
            return session;
        },
        [&server](mcp::ServerSession& session) { server.detach(session); });

    std::string error;
    if (!http.start(error)) {
        std::fprintf(stderr, "failed to start: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "listening on %u\n", http.port());
    std::fflush(stderr);

    for (;;) {
        ::pause();  // serve until killed
    }
}
