// Multi-session Streamable HTTP: Mcp-Session-Id issuance/validation, DELETE
// termination, idle expiry, and per-session isolation over real sockets.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <unistd.h>

#include <mcp/client/client.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_client_transport.hpp>
#include <mcp/transport/http_session_server.hpp>

#include "../src/transport/http/socket_util.hpp"
#include "../src/transport/line_io.hpp"

namespace {

using namespace mcp;

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
                      const std::string& extra_headers = "") {
    return raw_request(port,
                       "POST /mcp HTTP/1.1\r\nHost: t\r\n"
                       "Content-Type: application/json\r\n" +
                           extra_headers +
                           "Content-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body);
}

/// Extracts a header value from a raw response (exact-case name).
std::string response_header(const std::string& response, const std::string& name) {
    const auto at = response.find(name + ": ");
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + name.size() + 2;
    const auto end = response.find("\r\n", start);
    return response.substr(start, end - start);
}

const char* kInitializeBody =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
    R"("protocolVersion":"2025-11-25","capabilities":{},)"
    R"("clientInfo":{"name":"raw","version":"0"}}})";

struct SessionFixture {
    Server server{"session-test", "1.0.0"};
    std::unique_ptr<HttpSessionServer> http;

    void start(HttpSessionServerOptions options = {}) {
        ToolSpec echo;
        echo.description = "echo";
        echo.input_schema = json{{"type", "object"}};
        echo.handler = [](const json& args) -> CallToolResult {
            return {{text_content(args.value("message", "hi"))}};
        };
        ASSERT_TRUE(server.register_tool("echo", std::move(echo)));

        options.http.port = 0;
        http = std::make_unique<HttpSessionServer>(
            std::move(options),
            [this](std::shared_ptr<Transport> transport) {
                auto session = std::make_unique<ServerSession>(
                    std::move(transport), server.server_options());
                server.attach(*session);
                return session;
            },
            [this](ServerSession& session) { server.detach(session); });
        std::string error;
        ASSERT_TRUE(http->start(error)) << error;
        ASSERT_NE(http->port(), 0);
    }

    ~SessionFixture() {
        if (http) {
            http->stop();
        }
    }

    std::uint16_t port() const { return http->port(); }

    /// Raw initialize; returns the assigned session id.
    std::string raw_initialize() {
        const auto response = post_json(port(), kInitializeBody);
        EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos) << response;
        const auto sid = response_header(response, "Mcp-Session-Id");
        EXPECT_FALSE(sid.empty()) << response;
        return sid;
    }
};

HttpClientOptions client_options(std::uint16_t port) {
    HttpClientOptions options;
    options.port = port;
    options.reconnect_delay_ms = 100;
    return options;
}

TEST(HttpSession, InitializeAssignsSessionIdAndItWorks) {
    SessionFixture f;
    f.start();

    const auto sid = f.raw_initialize();
    EXPECT_EQ(sid.size(), 32u);
    EXPECT_EQ(f.http->session_count(), 1u);

    // The id unlocks the session: initialized notification then tools/list.
    const auto note = post_json(
        f.port(), R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
        "Mcp-Session-Id: " + sid + "\r\n");
    EXPECT_NE(note.find("HTTP/1.1 202"), std::string::npos) << note;

    const auto list =
        post_json(f.port(), R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})",
                  "Mcp-Session-Id: " + sid + "\r\n");
    EXPECT_NE(list.find("HTTP/1.1 200"), std::string::npos) << list;
    EXPECT_NE(list.find("\"echo\""), std::string::npos) << list;
}

TEST(HttpSession, MissingAndUnknownIdsRejected) {
    SessionFixture f;
    f.start();

    // Non-initialize POST without the header -> 400.
    const auto missing = post_json(
        f.port(), R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    EXPECT_NE(missing.find("HTTP/1.1 400"), std::string::npos) << missing;

    // Bogus id -> 404 (client should re-initialize).
    const auto bogus =
        post_json(f.port(), R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                  "Mcp-Session-Id: deadbeefdeadbeefdeadbeefdeadbeef\r\n");
    EXPECT_NE(bogus.find("HTTP/1.1 404"), std::string::npos) << bogus;

    // GET with bogus id -> 404 as well.
    const auto stream = raw_request(
        f.port(),
        "GET /mcp HTTP/1.1\r\nHost: t\r\nAccept: text/event-stream\r\n"
        "Mcp-Session-Id: deadbeefdeadbeefdeadbeefdeadbeef\r\n\r\n",
        300);
    EXPECT_NE(stream.find("HTTP/1.1 404"), std::string::npos) << stream;
}

TEST(HttpSession, DeleteTerminatesSession) {
    SessionFixture f;
    f.start();
    const auto sid = f.raw_initialize();

    const auto del = raw_request(
        f.port(), "DELETE /mcp HTTP/1.1\r\nHost: t\r\nMcp-Session-Id: " + sid +
                      "\r\nConnection: close\r\n\r\n");
    EXPECT_NE(del.find("HTTP/1.1 200"), std::string::npos) << del;
    EXPECT_EQ(f.http->session_count(), 0u);

    // The id is gone: further use -> 404.
    const auto after =
        post_json(f.port(), R"({"jsonrpc":"2.0","id":2,"method":"ping"})",
                  "Mcp-Session-Id: " + sid + "\r\n");
    EXPECT_NE(after.find("HTTP/1.1 404"), std::string::npos) << after;
}

TEST(HttpSession, DeleteCanBeDisabled) {
    SessionFixture f;
    HttpSessionServerOptions options;
    options.allow_client_termination = false;
    f.start(std::move(options));
    const auto sid = f.raw_initialize();

    const auto del = raw_request(
        f.port(), "DELETE /mcp HTTP/1.1\r\nHost: t\r\nMcp-Session-Id: " + sid +
                      "\r\nConnection: close\r\n\r\n");
    EXPECT_NE(del.find("HTTP/1.1 405"), std::string::npos) << del;
    EXPECT_EQ(f.http->session_count(), 1u);
}

TEST(HttpSession, IdleSessionsExpire) {
    SessionFixture f;
    HttpSessionServerOptions options;
    options.session_idle_timeout = std::chrono::milliseconds(150);
    f.start(std::move(options));
    const auto sid = f.raw_initialize();

    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // The sweep runs on request traffic; the stale id must 404.
    const auto after =
        post_json(f.port(), R"({"jsonrpc":"2.0","id":2,"method":"ping"})",
                  "Mcp-Session-Id: " + sid + "\r\n");
    EXPECT_NE(after.find("HTTP/1.1 404"), std::string::npos) << after;
    EXPECT_EQ(f.http->session_count(), 0u);
}

TEST(HttpSession, TwoConcurrentClientsAreIsolated) {
    SessionFixture f;
    f.start();

    Client alice("alice", "1.0");
    Client bob("bob", "1.0");
    auto alice_transport =
        std::make_shared<HttpClientTransport>(client_options(f.port()));
    auto bob_transport =
        std::make_shared<HttpClientTransport>(client_options(f.port()));

    auto alice_init = alice.connect(alice_transport);
    ASSERT_TRUE(alice_init) << alice_init.error().message;
    auto bob_init = bob.connect(bob_transport);
    ASSERT_TRUE(bob_init) << bob_init.error().message;

    // Distinct sessions.
    EXPECT_FALSE(alice_transport->session_id().empty());
    EXPECT_NE(alice_transport->session_id(), bob_transport->session_id());
    EXPECT_EQ(f.http->session_count(), 2u);

    auto a_call = alice.call_tool("echo", json{{"message", "from alice"}});
    ASSERT_TRUE(a_call) << a_call.error().message;
    auto b_call = bob.call_tool("echo", json{{"message", "from bob"}});
    ASSERT_TRUE(b_call) << b_call.error().message;
    EXPECT_EQ(std::get<TextContent>(a_call.value().content[0]).text,
              "from alice");
    EXPECT_EQ(std::get<TextContent>(b_call.value().content[0]).text,
              "from bob");

    // Alice leaving (DELETE on disconnect) must not affect Bob.
    alice.disconnect();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (f.http->session_count() > 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(f.http->session_count(), 1u);

    auto b_again = bob.call_tool("echo", json{{"message", "still here"}});
    ASSERT_TRUE(b_again) << b_again.error().message;
    bob.disconnect();
}

TEST(HttpSession, NotificationsFanOutToAllSessions) {
    SessionFixture f;
    f.start();

    std::mutex mutex;
    std::condition_variable cv;
    int alice_notes = 0;
    int bob_notes = 0;

    Client alice("alice", "1.0");
    alice.on_tools_list_changed([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++alice_notes;
        }
        cv.notify_all();
    });
    Client bob("bob", "1.0");
    bob.on_tools_list_changed([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++bob_notes;
        }
        cv.notify_all();
    });

    ASSERT_TRUE(alice.connect(
        std::make_shared<HttpClientTransport>(client_options(f.port()))));
    ASSERT_TRUE(bob.connect(
        std::make_shared<HttpClientTransport>(client_options(f.port()))));

    // Give both SSE streams a moment to attach, then mutate the registry.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ToolSpec extra;
    extra.input_schema = json{{"type", "object"}};
    extra.handler = [](const json&) { return CallToolResult{}; };
    ASSERT_TRUE(f.server.register_tool("extra", std::move(extra)));

    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] {
        return alice_notes >= 1 && bob_notes >= 1;
    })) << "alice=" << alice_notes << " bob=" << bob_notes;
    lock.unlock();

    alice.disconnect();
    bob.disconnect();
}

}  // namespace
