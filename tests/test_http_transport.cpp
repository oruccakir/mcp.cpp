// In-process Streamable HTTP tests over real sockets (FR-TRAN-005..009).

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <unistd.h>

#include <mcp/client/client.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_client_transport.hpp>
#include <mcp/transport/http_server_transport.hpp>

#include "../src/transport/http/socket_util.hpp"
#include "../src/transport/line_io.hpp"

namespace {

using namespace mcp;

/// Sends raw bytes, returns everything read until close/timeout.
std::string raw_request(std::uint16_t port, const std::string& text,
                        int idle_timeout_ms = 1000) {
    std::string error;
    const int fd = detail::connect_tcp("127.0.0.1", port, 2000, error);
    EXPECT_GE(fd, 0) << error;
    if (fd < 0) {
        return {};
    }
    EXPECT_TRUE(detail::write_all(fd, text.data(), text.size()));
    std::string out;
    char buffer[4096];
    for (;;) {
        if (detail::poll_readable(fd, -1, idle_timeout_ms) != 1) {
            break;
        }
        const long n = detail::read_some(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

std::string post_json(std::uint16_t port, const std::string& body,
                      const std::string& extra_headers = "",
                      const std::string& path = "/mcp") {
    return raw_request(
        port, "POST " + path + " HTTP/1.1\r\nHost: t\r\n" +
                  "Content-Type: application/json\r\n" + extra_headers +
                  "Content-Length: " + std::to_string(body.size()) +
                  "\r\nConnection: close\r\n\r\n" + body);
}

/// Server facade served over HttpServerTransport on an ephemeral port.
struct HttpServerFixture {
    Server server{"http-test-server", "1.0.0"};
    std::shared_ptr<HttpServerTransport> transport;
    std::thread thread;

    void register_echo() {
        ToolSpec echo;
        echo.description = "echo";
        echo.input_schema = json{{"type", "object"}};
        echo.handler = [](const json& args) -> CallToolResult {
            return {{text_content(args.value("message", "hi"))}};
        };
        ASSERT_TRUE(server.register_tool("echo", std::move(echo)));
    }

    void start(HttpServerOptions options = {}) {
        options.port = 0;
        transport = std::make_shared<HttpServerTransport>(std::move(options));
        thread = std::thread([this] { server.run(transport); });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (transport->port() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_NE(transport->port(), 0);
    }

    ~HttpServerFixture() {
        if (transport) {
            transport->disconnect();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    std::uint16_t port() const { return transport->port(); }
};

HttpClientOptions client_options(std::uint16_t port) {
    HttpClientOptions options;
    options.port = port;
    options.reconnect_delay_ms = 100;
    return options;
}

TEST(HttpTransport, FullStackHandshakeAndToolCall) {
    // FR-TEST-002: tool call round-trip over the HTTP transport.
    HttpServerFixture f;
    f.register_echo();
    f.start();

    Client client("http-client", "1.0.0");
    auto init = client.connect(
        std::make_shared<HttpClientTransport>(client_options(f.port())));
    ASSERT_TRUE(init) << init.error().message;
    EXPECT_EQ(init.value().server_info.name, "http-test-server");

    auto tools = client.list_tools();
    ASSERT_TRUE(tools) << tools.error().message;
    ASSERT_EQ(tools.value().items.size(), 1u);

    auto call = client.call_tool("echo", json{{"message", "over http"}});
    ASSERT_TRUE(call) << call.error().message;
    EXPECT_EQ(std::get<TextContent>(call.value().content[0]).text, "over http");

    EXPECT_TRUE(client.ping());
    client.disconnect();
}

TEST(HttpTransport, ServerNotificationArrivesViaSse) {
    // FR-TRAN-005/007: server-initiated messages flow over the GET stream.
    HttpServerFixture f;
    f.register_echo();
    f.start();

    std::mutex mutex;
    std::condition_variable cv;
    int list_changes = 0;

    Client client("http-client", "1.0.0");
    client.on_tools_list_changed([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++list_changes;
        }
        cv.notify_all();
    });
    auto init = client.connect(
        std::make_shared<HttpClientTransport>(client_options(f.port())));
    ASSERT_TRUE(init) << init.error().message;

    // Mutating the registry emits notifications/tools/list_changed -> SSE.
    ToolSpec extra;
    extra.input_schema = json{{"type", "object"}};
    extra.handler = [](const json&) { return CallToolResult{}; };
    ASSERT_TRUE(f.server.register_tool("extra", std::move(extra)));

    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return list_changes >= 1; }));
    lock.unlock();
    client.disconnect();
}

TEST(HttpTransport, NotificationPostAnswers202) {
    // FR-TRAN-006.
    HttpServerFixture f;
    f.start();
    const auto response = post_json(
        f.port(), R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    EXPECT_NE(response.find("HTTP/1.1 202"), std::string::npos) << response;
}

TEST(HttpTransport, BatchPostAnswersArray) {
    // FR-CORE-001 batches over HTTP; ping needs no handshake (FR-CORE-017).
    HttpServerFixture f;
    f.start();
    const auto response = post_json(
        f.port(),
        R"([{"jsonrpc":"2.0","id":1,"method":"ping"},)"
        R"({"jsonrpc":"2.0","id":2,"method":"ping"}])");
    ASSERT_NE(response.find("HTTP/1.1 200"), std::string::npos) << response;
    const auto body_at = response.find("\r\n\r\n");
    ASSERT_NE(body_at, std::string::npos);
    const auto body = json::parse(response.substr(body_at + 4));
    ASSERT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 2u);
}

TEST(HttpTransport, DisallowedOriginAnswers403) {
    // FR-TRAN-008.
    HttpServerFixture f;
    HttpServerOptions options;
    options.allowed_origins = {"https://app.example.com"};
    f.start(options);

    const auto rejected =
        post_json(f.port(), R"({"jsonrpc":"2.0","method":"x"})",
                  "Origin: https://evil.example.com\r\n");
    EXPECT_NE(rejected.find("HTTP/1.1 403"), std::string::npos) << rejected;

    const auto allowed =
        post_json(f.port(), R"({"jsonrpc":"2.0","method":"x"})",
                  "Origin: https://app.example.com\r\n");
    EXPECT_NE(allowed.find("HTTP/1.1 202"), std::string::npos) << allowed;

    const auto localhost_ok =
        post_json(f.port(), R"({"jsonrpc":"2.0","method":"x"})",
                  "Origin: http://localhost:5173\r\n");
    EXPECT_NE(localhost_ok.find("HTTP/1.1 202"), std::string::npos);
}

TEST(HttpTransport, AuthorizeHookAnswers401) {
    // FR-TRAN-008: authentication hook.
    HttpServerFixture f;
    HttpServerOptions options;
    options.authorize = [](const std::map<std::string, std::string>& headers) {
        const auto it = headers.find("authorization");
        return it != headers.end() && it->second == "Bearer sesame";
    };
    f.start(options);

    const auto denied =
        post_json(f.port(), R"({"jsonrpc":"2.0","method":"x"})");
    EXPECT_NE(denied.find("HTTP/1.1 401"), std::string::npos) << denied;

    const auto granted =
        post_json(f.port(), R"({"jsonrpc":"2.0","method":"x"})",
                  "Authorization: Bearer sesame\r\n");
    EXPECT_NE(granted.find("HTTP/1.1 202"), std::string::npos) << granted;
}

TEST(HttpTransport, WrongPathAnswers404) {
    HttpServerFixture f;
    f.start();
    const auto response = post_json(
        f.port(), R"({"jsonrpc":"2.0","method":"x"})", "", "/other");
    EXPECT_NE(response.find("HTTP/1.1 404"), std::string::npos) << response;
}

TEST(HttpTransport, InvalidJsonAnswers400WithParseError) {
    // FR-TRAN-006 + JSON-RPC: null-id -32700 body.
    HttpServerFixture f;
    f.start();
    const auto response = post_json(f.port(), "this is not json");
    ASSERT_NE(response.find("HTTP/1.1 400"), std::string::npos) << response;
    const auto body_at = response.find("\r\n\r\n");
    const auto body = json::parse(response.substr(body_at + 4));
    EXPECT_TRUE(body.at("id").is_null());
    EXPECT_EQ(body.at("error").at("code"), -32700);
}

TEST(HttpTransport, GetWithoutEventStreamAcceptRejected) {
    // FR-TRAN-005 content negotiation.
    HttpServerFixture f;
    f.start();
    const auto response = raw_request(
        f.port(),
        "GET /mcp HTTP/1.1\r\nHost: t\r\nAccept: application/json\r\n\r\n");
    EXPECT_NE(response.find("HTTP/1.1 406"), std::string::npos) << response;
}

TEST(HttpTransport, SseReplayWithLastEventId) {
    // FR-TRAN-009: resumability via globally unique, increasing event ids.
    auto transport = std::make_shared<HttpServerTransport>(HttpServerOptions{});
    transport->set_message_handler([](Message) {});
    transport->set_error_handler([](Error) {});
    transport->set_close_handler([] {});
    transport->connect();
    ASSERT_NE(transport->port(), 0);

    // Queue three server-initiated notifications -> ids 1..3.
    for (int i = 1; i <= 3; ++i) {
        transport->send(Message(JsonRpcNotification{
            "notifications/test", json{{"seq", i}}}));
    }

    const auto stream = raw_request(
        transport->port(),
        "GET /mcp HTTP/1.1\r\nHost: t\r\nAccept: text/event-stream\r\n"
        "Last-Event-ID: 1\r\n\r\n",
        500);
    EXPECT_NE(stream.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(stream.find("text/event-stream"), std::string::npos);
    EXPECT_NE(stream.find("retry: "), std::string::npos);   // FR-TRAN-007
    // Events 2 and 3 replayed; event 1 skipped.
    EXPECT_NE(stream.find("id: 2"), std::string::npos) << stream;
    EXPECT_NE(stream.find("id: 3"), std::string::npos) << stream;
    EXPECT_EQ(stream.find("\"seq\":1"), std::string::npos) << stream;
    EXPECT_NE(stream.find("\"seq\":2"), std::string::npos) << stream;

    transport->disconnect();
}

TEST(HttpTransport, ClientReconnectsWithLastEventId) {
    // FR-TRAN-007: stream drop -> reconnect with Last-Event-ID.
    HttpServerOptions server_options;
    server_options.sse_retry_ms = 100;  // reconnect fast in tests
    auto server_transport =
        std::make_shared<HttpServerTransport>(server_options);
    server_transport->set_message_handler([](Message) {});
    server_transport->set_error_handler([](Error) {});
    server_transport->set_close_handler([] {});
    server_transport->connect();
    ASSERT_NE(server_transport->port(), 0);

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int> received;

    HttpClientOptions options = client_options(server_transport->port());
    auto client_transport = std::make_shared<HttpClientTransport>(options);
    client_transport->set_message_handler([&](Message message) {
        const auto& note = std::get<JsonRpcNotification>(message);
        {
            std::lock_guard<std::mutex> lock(mutex);
            received.push_back(note.params->at("seq").get<int>());
        }
        cv.notify_all();
    });
    client_transport->set_error_handler([](Error) {});
    client_transport->set_close_handler([] {});
    client_transport->connect();

    server_transport->send(Message(JsonRpcNotification{
        "notifications/test", json{{"seq", 1}}}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                                [&] { return received.size() >= 1; }));
    }

    // A new GET takes the stream over, dropping the client's connection;
    // the client must reconnect (with Last-Event-ID) and keep receiving.
    std::thread usurper([&] {
        raw_request(server_transport->port(),
                    "GET /mcp HTTP/1.1\r\nHost: t\r\n"
                    "Accept: text/event-stream\r\n\r\n",
                    300);
    });
    usurper.join();

    // Give the client time to win the stream back, then send event 2.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    server_transport->send(Message(JsonRpcNotification{
        "notifications/test", json{{"seq", 2}}}));

    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                                [&] { return received.size() >= 2; }));
        EXPECT_EQ(received.back(), 2);
    }

    client_transport->disconnect();
    server_transport->disconnect();
}

}  // namespace
