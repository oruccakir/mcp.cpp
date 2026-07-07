// End-to-end: spawn examples/echo_server_http as a real subprocess and run
// the full MCP initialize handshake and tool round-trip over the Streamable
// HTTP transport (FR-TRAN-005..009, FR-TEST-002).

#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <future>
#include <memory>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <sys/wait.h>

#include <mcp/client/client.hpp>
#include <mcp/content.hpp>
#include <mcp/json.hpp>
#include <mcp/transport/http_client_transport.hpp>

namespace {

using namespace mcp;

/// Forks echo_server_http --port 0, reads the bound port from the child's
/// "listening on http://127.0.0.1:<port>/mcp" stderr line, and owns the pid.
struct HttpServerProc {
    pid_t pid = -1;
    std::uint16_t port = 0;
    int stderr_fd = -1;
    std::string diag;

    bool start() {
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;
        pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDERR_FILENO);
            ::close(pipefd[1]);
            std::string path = ECHO_SERVER_HTTP_PATH;
            std::vector<char> path_buf(path.begin(), path.end());
            path_buf.push_back('\0');
            std::string flag = "--port";
            std::string val = "0";
            char* argv[] = {path_buf.data(), flag.data(), val.data(), nullptr};
            ::execv(path_buf.data(), argv);
            ::_exit(127);
        }
        ::close(pipefd[1]);
        stderr_fd = pipefd[0];
        return read_port();
    }

    bool read_port() {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
        char buf[256];
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd p{stderr_fd, POLLIN, 0};
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (::poll(&p, 1, static_cast<int>(rem.count())) <= 0) break;
            ssize_t n = ::read(stderr_fd, buf, sizeof(buf));
            if (n <= 0) break;
            diag.append(buf, static_cast<std::size_t>(n));
            auto pos = diag.find("listening on http://127.0.0.1:");
            if (pos != std::string::npos) {
                auto colon = diag.find(':', pos + 28);
                if (colon != std::string::npos) {
                    std::string num;
                    for (std::size_t i = colon + 1;
                         i < diag.size() &&
                         ::isdigit(static_cast<unsigned char>(diag[i]));
                         ++i)
                        num += diag[i];
                    if (!num.empty()) {
                        port = static_cast<std::uint16_t>(std::stoi(num));
                        return true;
                    }
                }
            }
        }
        return false;
    }

    ~HttpServerProc() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
        if (stderr_fd >= 0) ::close(stderr_fd);
    }
};

TEST(HttpIntegration, FullHandshakeAndEchoRoundTrip) {
    HttpServerProc srv;
    ASSERT_TRUE(srv.start()) << "echo_server_http did not listen: " << srv.diag;
    ASSERT_GT(srv.port, 0);

    HttpClientOptions copts;
    copts.host = "127.0.0.1";
    copts.port = srv.port;
    copts.reconnect_delay_ms = std::chrono::milliseconds(200);
    auto transport = std::make_shared<HttpClientTransport>(copts);

    Client client("integration-test", "1.0.0");
    auto init = client.connect(transport);
    ASSERT_TRUE(init) << init.error().message;
    EXPECT_EQ(init.value().server_info.name, "echo-server-http");
    EXPECT_EQ(init.value().protocol_version, kProtocolVersion);
    ASSERT_TRUE(client.server_capabilities().has_value());
    EXPECT_TRUE(client.server_capabilities()->tools.has_value());

    auto tools = client.list_tools();
    ASSERT_TRUE(tools) << tools.error().message;
    ASSERT_EQ(tools.value().items.size(), 1u);
    EXPECT_EQ(tools.value().items[0].name, "echo");

    auto call =
        client.call_tool("echo", json{{"message", "hello-from-http"}});
    ASSERT_TRUE(call) << call.error().message;
    ASSERT_EQ(call.value().content.size(), 1u);
    auto* text = std::get_if<TextContent>(&call.value().content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "hello-from-http");

    // FR-CORE-017: ping health check over HTTP.
    auto pong = client.ping();
    ASSERT_TRUE(pong) << pong.error().message;

    client.disconnect();
}

}  // namespace