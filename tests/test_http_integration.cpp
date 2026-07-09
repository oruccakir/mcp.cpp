// End-to-end Streamable HTTP: spawn examples/echo_server_http as a real
// subprocess and drive it with mcp::Client over HttpClientTransport
// (FR-TEST-002). The subprocess is managed via StdioClientTransport purely
// for spawning/stderr capture/shutdown escalation; MCP flows over HTTP.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>

#include <mcp/client/client.hpp>
#include <mcp/transport/http_client_transport.hpp>
#include <mcp/transport/stdio_client_transport.hpp>

namespace {

using namespace mcp;

#if defined(ECHO_SERVER_HTTP_PATH)  // requires MCP_BUILD_EXAMPLES
TEST(HttpIntegration, EchoServerOverRealHttp) {
    StdioServerParameters parameters;
    parameters.command = ECHO_SERVER_HTTP_PATH;
    parameters.args = {"--port", "0"};  // ephemeral; announced on stderr

    auto process = std::make_shared<StdioClientTransport>(parameters);
    auto port_promise = std::make_shared<std::promise<std::uint16_t>>();
    auto port_future = port_promise->get_future();
    auto announced = std::make_shared<std::atomic<bool>>(false);
    process->set_stderr_handler([port_promise, announced](std::string line) {
        const std::string prefix = "listening on ";
        if (line.rfind(prefix, 0) == 0 && !announced->exchange(true)) {
            port_promise->set_value(static_cast<std::uint16_t>(
                std::atoi(line.c_str() + prefix.size())));
        }
    });
    process->set_message_handler([](Message) {});
    process->set_error_handler([](Error) {});
    process->set_close_handler([] {});
    process->connect();

    ASSERT_EQ(port_future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready)
        << "server never announced its port";
    const auto port = port_future.get();
    ASSERT_NE(port, 0);

    HttpClientOptions options;
    options.port = port;
    options.reconnect_delay_ms = 200;

    Client client("http-integration", "1.0.0");
    auto init =
        client.connect(std::make_shared<HttpClientTransport>(options));
    ASSERT_TRUE(init) << init.error().message;
    EXPECT_EQ(init.value().server_info.name, "echo-server-http");
    EXPECT_EQ(init.value().protocol_version, kProtocolVersion);

    auto tools = client.list_tools();
    ASSERT_TRUE(tools) << tools.error().message;
    ASSERT_EQ(tools.value().items.size(), 1u);
    EXPECT_EQ(tools.value().items[0].name, "echo");

    auto call = client.call_tool("echo", json{{"message", "http subprocess"}});
    ASSERT_TRUE(call) << call.error().message;
    EXPECT_EQ(std::get<TextContent>(call.value().content[0]).text,
              "http subprocess");

    EXPECT_TRUE(client.ping());

    client.disconnect();
    process->disconnect();  // close stdin -> SIGTERM -> SIGKILL (FR-TRAN-004)
}
#endif  // ECHO_SERVER_HTTP_PATH

}  // namespace
