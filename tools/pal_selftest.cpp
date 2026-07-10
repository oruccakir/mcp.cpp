// pal_selftest: zero-dependency PAL + full-stack verifier for new platform
// backends (designed for air-gapped bring-up: plain printf PASS/FAIL, exits
// nonzero on any failure — the whole output fits in one screen/photo).
//
// Run it on the target (e.g. as a VxWorks RTP). No GTest, no network access
// beyond 127.0.0.1 loopback.

#include <cstdio>
#include <cstring>
#include <memory>

#include <mcp/mcp.hpp>
#include <mcp/version.hpp>

#include "../src/platform/pal.hpp"

namespace {

int g_failures = 0;

void report(const char* name, bool ok, const char* detail = "") {
    if (ok) {
        std::printf("[ OK ] %s\n", name);
    } else {
        ++g_failures;
        std::printf("[FAIL] %s %s(errno=%d)\n", name, detail, errno);
    }
    std::fflush(stdout);
}

void test_wake_event() {
    mcp::pal::WakeEvent wake;
    report("wake_event.create", wake.valid());
    if (!wake.valid()) {
        return;
    }
    // Unsignalled: poll on the handle must time out (0), not fire.
    int rc = mcp::pal::poll_readable(wake.poll_handle(), nullptr, 50);
    report("wake_event.silent_before_signal", rc == 0);
    wake.signal();
    rc = mcp::pal::poll_readable(wake.poll_handle(), nullptr, 1000);
    report("wake_event.signal_wakes_poll", rc == 1);
}

void test_tcp_loopback() {
    std::string error;
    mcp::pal::fd_t listener = mcp::pal::tcp_listen("127.0.0.1", 0, error);
    report("tcp.listen_ephemeral", listener != mcp::pal::kInvalidFd,
           error.c_str());
    if (listener == mcp::pal::kInvalidFd) {
        return;
    }
    const std::uint16_t port = mcp::pal::tcp_local_port(listener);
    report("tcp.local_port", port != 0);

    mcp::pal::fd_t client =
        mcp::pal::tcp_connect("127.0.0.1", port, 2000, error);
    report("tcp.connect", client != mcp::pal::kInvalidFd, error.c_str());

    mcp::pal::fd_t served = mcp::pal::tcp_accept(listener);
    report("tcp.accept", served != mcp::pal::kInvalidFd);

    if (client != mcp::pal::kInvalidFd && served != mcp::pal::kInvalidFd) {
        const char message[] = "mcp-selftest";
        report("tcp.write_all",
               mcp::pal::write_all(client, message, sizeof(message)));
        char buffer[64] = {};
        const int ready = mcp::pal::poll_readable(served, nullptr, 2000);
        report("tcp.poll_readable", ready == 1);
        const long n = mcp::pal::read_some(served, buffer, sizeof(buffer));
        report("tcp.read_some_roundtrip",
               n == static_cast<long>(sizeof(message)) &&
                   std::strcmp(buffer, message) == 0);
        mcp::pal::shutdown_fd(client);
        const long eof = mcp::pal::read_some(served, buffer, sizeof(buffer));
        report("tcp.shutdown_gives_eof", eof == 0);
    }
    mcp::pal::close_fd(client);
    mcp::pal::close_fd(served);
    mcp::pal::close_fd(listener);
}

void test_full_stack() {
    // Threads, timers, sessions, HTTP + SSE — the whole SDK on loopback.
    mcp::Server server("selftest-server", MCP_CPP_VERSION);
    mcp::ToolSpec echo;
    echo.input_schema = mcp::json{{"type", "object"}};
    echo.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        return {{mcp::text_content(args.value("message", "hi"))}};
    };
    report("stack.register_tool",
           static_cast<bool>(server.register_tool("echo", std::move(echo))));

    mcp::HttpSessionServerOptions options;
    options.http.port = 0;
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
    report("stack.http_start", http.start(error), error.c_str());
    if (http.port() == 0) {
        return;
    }
    std::printf("       (listening on 127.0.0.1:%u)\n", http.port());

    mcp::HttpClientOptions client_options;
    client_options.port = http.port();
    client_options.reconnect_delay_ms = 200;
    mcp::Client client("selftest-client", MCP_CPP_VERSION);
    auto init = client.connect(
        std::make_shared<mcp::HttpClientTransport>(client_options));
    report("stack.initialize", static_cast<bool>(init),
           init ? "" : init.error().message.c_str());

    auto tools = client.list_tools();
    report("stack.tools_list",
           tools && tools.value().items.size() == 1 &&
               tools.value().items[0].name == "echo");

    auto call = client.call_tool("echo", mcp::json{{"message", "vxworks"}});
    bool call_ok = false;
    if (call && !call.value().content.empty()) {
        const auto* text =
            std::get_if<mcp::TextContent>(&call.value().content[0]);
        call_ok = text != nullptr && text->text == "vxworks";
    }
    report("stack.tools_call_roundtrip", call_ok);

    client.disconnect();
    report("stack.session_delete", http.session_count() == 0);
    http.stop();
}

}  // namespace

int main() {
    std::printf("mcp.cpp %s pal_selftest\n", MCP_CPP_VERSION);
    std::printf("----------------------------------------\n");
    test_wake_event();
    test_tcp_loopback();
    test_full_stack();
    std::printf("----------------------------------------\n");
    if (g_failures == 0) {
        std::printf("RESULT: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("RESULT: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
