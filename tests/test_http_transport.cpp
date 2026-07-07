// In-process Streamable HTTP transport tests (FR-TRAN-005..009, FR-TEST-002).
// Real sockets on ephemeral ports; the Server facade is served by
// HttpServerTransport and driven by Client over HttpClientTransport, with
// selected cases exercised via raw sockets.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mcp/client/client.hpp>
#include <mcp/content.hpp>
#include <mcp/json.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_client_transport.hpp>
#include <mcp/transport/http_server_transport.hpp>

#include "../src/transport/http/http_codec.hpp"

namespace {

using namespace mcp;
using detail::http::HttpResponse;
using detail::http::HttpResponseParser;
using detail::http::ParseStatus;
using detail::http::SseEvent;
using detail::http::SseParser;

constexpr auto kTimeout = std::chrono::milliseconds(5000);

/// Sink collecting client-side messages with wait helpers.
struct Sink {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Message> messages;
    std::vector<Error> errors;
    std::optional<std::string> last_event_id;

    void attach(Transport& t) {
        t.set_message_handler([this](Message m) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                messages.push_back(std::move(m));
            }
            cv.notify_all();
        });
        t.set_error_handler([this](Error e) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                errors.push_back(std::move(e));
            }
            cv.notify_all();
        });
    }

    template <typename Pred>
    bool wait(Pred pred, std::chrono::milliseconds timeout = kTimeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, pred);
    }
};

int connect_to(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)), 0);
    return fd;
}

bool write_all(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

/// Sends a raw HTTP request and reads a complete response.
HttpResponse raw_request(std::uint16_t port, const std::string& request) {
    int fd = connect_to(port);
    write_all(fd, request);
    HttpResponseParser parser;
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(remaining.count())) <= 0) break;
        char chunk[4096];
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        auto st = parser.feed(chunk, static_cast<std::size_t>(n));
        if (st == ParseStatus::Done || st == ParseStatus::Error) break;
    }
    ::close(fd);
    return parser.response();
}

/// Reads SSE events from a raw GET connection until `pred` is satisfied or
/// timeout. Returns the parsed events (with non-empty data only).
std::vector<SseEvent> raw_sse(std::uint16_t port, const std::string& request,
                              std::chrono::milliseconds timeout = kTimeout) {
    int fd = connect_to(port);
    write_all(fd, request);
    SseParser sse;
    std::vector<SseEvent> events;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(remaining.count())) <= 0) break;
        char chunk[4096];
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        auto got = sse.feed(chunk, static_cast<std::size_t>(n));
        for (auto& e : got) events.push_back(std::move(e));
    }
    ::close(fd);
    return events;
}

std::string post_request(const std::string& path, const std::string& body,
                         std::optional<std::string> origin = std::nullopt,
                         std::optional<std::string> last_event_id = std::nullopt) {
    std::string h =
        "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
        "application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n";
    if (origin) h += "Origin: " + *origin + "\r\n";
    if (last_event_id) h += "Last-Event-ID: " + *last_event_id + "\r\n";
    h += "\r\n" + body;
    return h;
}

std::string get_request(const std::string& path,
                        std::optional<std::string> last_event_id = std::nullopt,
                        std::optional<std::string> origin = std::nullopt) {
    std::string h = "GET " + path +
                    " HTTP/1.1\r\nHost: localhost\r\nAccept: "
                    "text/event-stream\r\n";
    if (last_event_id) h += "Last-Event-ID: " + *last_event_id + "\r\n";
    if (origin) h += "Origin: " + *origin + "\r\n";
    h += "\r\n";
    return h;
}

/// Echo Server on an ephemeral HTTP port, started on a background thread.
struct EchoServer {
    Server server{"echo-http", "1.0.0"};
    std::shared_ptr<HttpServerTransport> transport;
    std::thread thread;
    std::uint16_t port = 0;

    EchoServer() {
        ToolSpec echo;
        echo.description = "Echoes back the input";
        echo.input_schema =
            json{{"type", "object"},
                 {"properties", {{"message", {{"type", "string"}}}}},
                 {"required", json::array({"message"})}};
        echo.handler = [](const json& a) -> CallToolResult {
            return {{text_content(a.at("message").get<std::string>())}};
        };
        server.register_tool("echo", std::move(echo));

        HttpServerOptions opts;
        opts.port = 0;
        transport = std::make_shared<HttpServerTransport>(opts);
        transport->connect();
        port = transport->port();
        thread = std::thread([this] { server.run(transport); });
    }

    ~EchoServer() {
        transport->disconnect();
        if (thread.joinable()) thread.join();
    }
};

TEST(HttpTransport, HandshakeAndToolRoundTrip) {
    EchoServer srv;
    Sink sink;

    HttpClientOptions copts;
    copts.host = "127.0.0.1";
    copts.port = srv.port;
    copts.reconnect_delay_ms = std::chrono::milliseconds(100);
    auto ct = std::make_shared<HttpClientTransport>(copts);
    sink.attach(*ct);

    Client client("test-client", "1.0.0");
    client.on_tools_list_changed([&sink] {
        // notifications arrive via SSE; not expected in this test.
    });
    auto init = client.connect(ct);
    ASSERT_TRUE(init) << init.error().message;
    EXPECT_EQ(init.value().server_info.name, "echo-http");
    ASSERT_TRUE(client.server_capabilities().has_value());
    EXPECT_TRUE(client.server_capabilities()->tools.has_value());

    auto tools = client.list_tools();
    ASSERT_TRUE(tools) << tools.error().message;
    ASSERT_EQ(tools.value().items.size(), 1u);
    EXPECT_EQ(tools.value().items[0].name, "echo");

    auto call = client.call_tool("echo", json{{"message", "hello-http"}});
    ASSERT_TRUE(call) << call.error().message;
    ASSERT_EQ(call.value().content.size(), 1u);
    auto* text = std::get_if<TextContent>(&call.value().content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "hello-http");

    client.disconnect();
}

TEST(HttpTransport, Accepted202ForBareNotification) {
    EchoServer srv;
    // notifications/initialized has no id -> server answers 202.
    const std::string body =
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    auto res = raw_request(srv.port, post_request("/mcp", body));
    EXPECT_EQ(res.status, 202);
}

TEST(HttpTransport, ServerInitiatedNotificationViaSse) {
    EchoServer srv;
    std::mutex mutex;
    std::condition_variable cv;
    bool got_changed = false;

    HttpClientOptions copts;
    copts.host = "127.0.0.1";
    copts.port = srv.port;
    copts.reconnect_delay_ms = std::chrono::milliseconds(100);
    auto ct = std::make_shared<HttpClientTransport>(copts);

    Client client("sse-client", "1.0.0");
    client.on_tools_list_changed([&] {
        std::lock_guard<std::mutex> lock(mutex);
        got_changed = true;
        cv.notify_all();
    });
    auto init = client.connect(ct);
    ASSERT_TRUE(init) << init.error().message;

    // Registering a new tool after operating fires tools/list_changed over SSE.
    ToolSpec probe;
    probe.description = "probe";
    probe.handler = [](const json&) -> CallToolResult { return {}; };
    srv.server.register_tool("probe", std::move(probe));

    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, kTimeout, [&] { return got_changed; }));
    }
    client.disconnect();
}

TEST(HttpTransport, ResumabilityReplaysAfterLastEventId) {
    // Bare transport: enqueue three server-initiated events with no stream
    // attached, then resume from Last-Event-ID and check replay order/ids.
    HttpServerOptions opts;
    opts.port = 0;
    HttpServerTransport server(opts);
    server.connect();
    const std::uint16_t port = server.port();

    server.send(Message(JsonRpcNotification{"n1", json::object()}));  // id 1
    server.send(Message(JsonRpcNotification{"n2", json::object()}));  // id 2
    server.send(Message(JsonRpcNotification{"n3", json::object()}));  // id 3

    auto events = raw_sse(port, get_request("/mcp", std::optional<std::string>{"1"}),
                          std::chrono::milliseconds(2000));
    std::vector<std::pair<std::int64_t, std::string>> msgs;
    for (auto& e : events) {
        if (!e.data.empty() && e.id) {
            msgs.emplace_back(std::stoll(*e.id), e.data);
        }
    }
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].first, 2);
    EXPECT_EQ(msgs[1].first, 3);
    EXPECT_NE(msgs[0].second.find("n2"), std::string::npos);
    EXPECT_NE(msgs[1].second.find("n3"), std::string::npos);

    server.disconnect();
}

TEST(HttpTransport, SecurityDisallowedOrigin) {
    HttpServerOptions opts;
    opts.port = 0;
    HttpServerTransport server(opts);
    server.connect();
    const std::uint16_t port = server.port();
    auto res = raw_request(port, post_request("/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                                              std::optional<std::string>{"http://evil.example"}));
    EXPECT_EQ(res.status, 403);
    server.disconnect();
}

TEST(HttpTransport, SecurityAuthorizeRejected) {
    HttpServerOptions opts;
    opts.port = 0;
    opts.authorize = [](const HttpRequestView&) { return false; };
    HttpServerTransport server(opts);
    server.connect();
    const std::uint16_t port = server.port();
    auto res = raw_request(port, post_request("/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})"));
    EXPECT_EQ(res.status, 401);
    server.disconnect();
}

TEST(HttpTransport, SecurityWrongPath) {
    HttpServerOptions opts;
    opts.port = 0;
    HttpServerTransport server(opts);
    server.connect();
    const std::uint16_t port = server.port();
    auto res = raw_request(port, post_request("/nope", R"({"jsonrpc":"2.0","id":1,"method":"ping"})"));
    EXPECT_EQ(res.status, 404);
    server.disconnect();
}

TEST(HttpTransport, SecurityInvalidJsonIs400ParseError) {
    HttpServerOptions opts;
    opts.port = 0;
    HttpServerTransport server(opts);
    server.connect();
    const std::uint16_t port = server.port();
    auto res = raw_request(port, post_request("/mcp", "not json"));
    EXPECT_EQ(res.status, 400);
    // Body is a null-id JSON-RPC ParseError response.
    auto frame = parse_frame(res.body);
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame.value().messages.size(), 1u);
    auto* r = std::get_if<JsonRpcResponse>(&frame.value().messages[0]);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(r->id.has_value());
    ASSERT_TRUE(r->error.has_value());
    EXPECT_EQ(r->error->code, -32700);
    server.disconnect();
}

TEST(HttpTransport, SecurityGetRequiresEventStreamAccept) {
    HttpServerOptions opts;
    opts.port = 0;
    HttpServerTransport server(opts);
    server.connect();
    const std::uint16_t port = server.port();
    std::string req =
        "GET /mcp HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n\r\n";
    auto res = raw_request(port, req);
    EXPECT_EQ(res.status, 406);
    server.disconnect();
}

TEST(HttpTransport, ClientSseReconnect) {
    // Full facades: the handshake guarantees the client's SSE stream is
    // attached before any server-initiated message flows. After receiving E1,
    // a second (raw) GET replaces the client's stream (dropping it); the client
    // reconnects with Last-Event-ID and receives E2 queued during the outage
    // (FR-TRAN-007/009).
    EchoServer srv;
    std::mutex mutex;
    std::condition_variable cv;
    int changes = 0;

    HttpClientOptions copts;
    copts.host = "127.0.0.1";
    copts.port = srv.port;
    copts.reconnect_delay_ms = std::chrono::milliseconds(150);
    auto ct = std::make_shared<HttpClientTransport>(copts);

    Client client("reconnect-client", "1.0.0");
    client.on_tools_list_changed([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++changes;
        cv.notify_all();
    });
    auto init = client.connect(ct);
    ASSERT_TRUE(init) << init.error().message;

    // E1: registering a tool fires tools/list_changed over the live SSE stream.
    ToolSpec probe1;
    probe1.description = "probe1";
    probe1.handler = [](const json&) -> CallToolResult { return {}; };
    srv.server.register_tool("probe1", std::move(probe1));
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, kTimeout, [&] { return changes >= 1; }));
    }

    // Drop the client's stream by opening a competing GET (newest wins).
    {
        int dropper = connect_to(srv.port);
        write_all(dropper, get_request("/mcp"));
        // E2: queued while the client is reconnecting (id > last-event-id).
        ToolSpec probe2;
        probe2.description = "probe2";
        probe2.handler = [](const json&) -> CallToolResult { return {}; };
        srv.server.register_tool("probe2", std::move(probe2));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::close(dropper);
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, kTimeout, [&] { return changes >= 2; }));
    }
    EXPECT_GE(changes, 2);
    client.disconnect();
}

}  // namespace