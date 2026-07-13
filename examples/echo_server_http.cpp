// Echo server over the multi-session Streamable HTTP transport
// (FR-TRAN-005..009 + Mcp-Session-Id management).
//
// Host / VxWorks RTP (has a main + argv):
//   ./echo_server_http --port 3001 [--host 0.0.0.0]
//
// VxWorks DKM (kernel module — no argv, main() must not block the shell):
//   -> ld < echo_server_http.out
//   -> mcp_echo_http_start 3001     # binds 0.0.0.0:<port>, returns immediately
//   -> mcp_echo_http_stop           # stops and frees
// (needs the ANSI stdio VSB component for the printf status lines).
//
// TLS is out of scope; bind stays on 127.0.0.1 unless overridden.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <mcp/mcp.hpp>

namespace {

void configure_echo(mcp::Server& server) {
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
}

std::unique_ptr<mcp::HttpSessionServer> make_http(
    mcp::Server& server, mcp::HttpSessionServerOptions options) {
    return std::make_unique<mcp::HttpSessionServer>(
        options,
        [&server](std::shared_ptr<mcp::Transport> transport) {
            auto session = std::make_unique<mcp::ServerSession>(
                std::move(transport), server.server_options());
            server.attach(*session);
            return session;
        },
        [&server](mcp::ServerSession& session) { server.detach(session); });
}

}  // namespace

int main(int argc, char** argv) {
    mcp::HttpSessionServerOptions options;
    options.http.port = 3002;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) {
            options.http.port =
                static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--host") == 0) {
            // Default stays 127.0.0.1 (FR-TRAN-008). Binding 0.0.0.0 exposes
            // the server to the network: front it with a TLS-terminating
            // reverse proxy and configure allowed_origins / authorize.
            options.http.host = argv[i + 1];
        }
    }

    mcp::Server server("echo-server-http", "1.0.0");
    configure_echo(server);
    auto http = make_http(server, options);

    std::string error;
    if (!http->start(error)) {
        std::fprintf(stderr, "failed to start: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "listening on %u\n", http->port());
    std::fflush(stderr);

    // Serve until killed. A plain sleep loop (no std::promise/future) so this
    // links without pthreads — relevant for the VxWorks builds.
    for (;;) {
        mcp::sys::this_thread::sleep_for(std::chrono::hours(1));
    }
    return 0;
}

// --- VxWorks DKM entry points ------------------------------------------------
// The kernel shell cannot pass an argv array or block on main(), so expose
// no-argv C entries. HttpSessionServer::start() is non-blocking (its accept
// task keeps serving), so the start entry returns immediately; the server
// objects are kept alive in file-scope owners until stop.

namespace {
std::unique_ptr<mcp::Server> g_dkm_server;
std::unique_ptr<mcp::HttpSessionServer> g_dkm_http;
}  // namespace

extern "C" int mcp_echo_http_start(int port) {
    if (g_dkm_http) {
        std::printf("echo_server_http already running on port %u\n",
                    g_dkm_http->port());
        return 0;
    }
    g_dkm_server = std::make_unique<mcp::Server>("echo-server-http", "1.0.0");
    configure_echo(*g_dkm_server);

    mcp::HttpSessionServerOptions options;
    options.http.host = "0.0.0.0";  // reachable from the network
    options.http.port = static_cast<std::uint16_t>(port);
    g_dkm_http = make_http(*g_dkm_server, options);

    std::string error;
    if (!g_dkm_http->start(error)) {
        std::printf("echo_server_http start failed: %s\n", error.c_str());
        g_dkm_http.reset();
        g_dkm_server.reset();
        return -1;
    }
    std::printf("echo_server_http listening on 0.0.0.0:%u\n",
                g_dkm_http->port());
    return 0;
}

extern "C" int mcp_echo_http_stop(void) {
    if (g_dkm_http) {
        g_dkm_http->stop();  // joins the accept task before we free the server
        g_dkm_http.reset();
        g_dkm_server.reset();
        std::printf("echo_server_http stopped\n");
    }
    return 0;
}
