// Echo server over the Streamable HTTP transport (FR-TRAN-005..009).
//
//   ./echo_server_http --port 3001
//   curl -s -X POST http://127.0.0.1:3001/mcp -H 'Content-Type: application/json'
//        -H 'Accept: application/json' -d '<initialize request JSON>'
//   curl -N http://127.0.0.1:3001/mcp -H 'Accept: text/event-stream'
//
// TLS is out of scope; bind stays on 127.0.0.1 unless fronted by a proxy.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include <mcp/mcp.hpp>

int main(int argc, char** argv) {
    mcp::HttpServerOptions options;
    options.port = 3001;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) {
            options.port = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
        }
    }

    mcp::Server server("echo-server-http", "1.0.0");
    // Sessionless HTTP: every reconnecting client re-runs initialize.
    server.set_allow_reinitialize(true);
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

    auto transport = std::make_shared<mcp::HttpServerTransport>(options);
    transport->set_error_handler([](const mcp::Error& e) {
        std::fprintf(stderr, "transport error: %s\n", e.message.c_str());
    });

    // Server::run wires the session and calls connect(); poll for the bound
    // port so integration tests can parse it from stderr.
    std::thread announce([&transport] {
        while (transport->port() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::fprintf(stderr, "listening on %u\n", transport->port());
        std::fflush(stderr);
    });
    const int rc = server.run(transport);
    announce.join();
    return rc;
}
