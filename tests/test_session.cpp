#include <gtest/gtest.h>

#include <future>
#include <thread>

#include <mcp/core/session.hpp>
#include <mcp/methods.hpp>

#include "mock_transport.hpp"

namespace {

using namespace mcp;
using mcp_test::MockTransport;

Message make_initialize(std::int64_t id,
                        const std::string& version = kProtocolVersion) {
    InitializeParams params{version, ClientCapabilities{},
                            Implementation{"test-client", std::nullopt, "1.0"}};
    return Message(JsonRpcRequest{RequestId(id), methods::kInitialize,
                                  json(params)});
}

Message make_initialized() {
    return Message(JsonRpcNotification{methods::kNotificationInitialized, {}});
}

const JsonRpcResponse& as_response(const Message& m) {
    return std::get<JsonRpcResponse>(m);
}

struct ServerFixture {
    std::shared_ptr<MockTransport> transport = std::make_shared<MockTransport>();
    ServerSession session;

    explicit ServerFixture(ServerOptions options = default_options())
        : session(transport, std::move(options)) {
        session.connect();
    }

    static ServerOptions default_options() {
        ServerOptions options;
        options.server_info = Implementation{"test-server", std::nullopt, "0.1.0"};
        return options;
    }

    void handshake() {
        transport->deliver(make_initialize(1));
        ASSERT_TRUE(transport->wait_for_sent(0).has_value());
        transport->deliver(make_initialized());
        ASSERT_EQ(session.state(), SessionState::Operating);
    }
};

TEST(ServerSession, HandshakeSequence) {
    // FR-CORE-010: initialize -> response -> initialized notification.
    ServerFixture f;
    EXPECT_EQ(f.session.state(), SessionState::Uninitialized);

    f.transport->deliver(make_initialize(1));
    auto sent = f.transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto& response = as_response(*sent);
    EXPECT_FALSE(response.is_error());
    EXPECT_EQ(std::get<std::int64_t>(*response.id), 1);

    const auto result = response.result->get<InitializeResult>();
    EXPECT_EQ(result.protocol_version, kProtocolVersion);
    EXPECT_EQ(result.server_info.name, "test-server");
    EXPECT_EQ(f.session.state(), SessionState::Initializing);

    f.transport->deliver(make_initialized());
    EXPECT_EQ(f.session.state(), SessionState::Operating);

    ASSERT_TRUE(f.session.client_info().has_value());
    EXPECT_EQ(f.session.client_info()->name, "test-client");
}

TEST(ServerSession, VersionMismatchOffersServerVersion) {
    // FR-CORE-006: unsupported requested version -> server offers its own.
    ServerFixture f;
    f.transport->deliver(make_initialize(1, "1999-01-01"));
    auto sent = f.transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto result = as_response(*sent).result->get<InitializeResult>();
    EXPECT_EQ(result.protocol_version, kProtocolVersion);
}

TEST(ServerSession, RequestBeforeInitializeRejected) {
    // FR-CORE-005/-32000.
    ServerFixture f;
    f.transport->deliver(
        Message(JsonRpcRequest{RequestId(std::int64_t{5}), "tools/list", {}}));
    auto sent = f.transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto& response = as_response(*sent);
    ASSERT_TRUE(response.is_error());
    EXPECT_EQ(response.error->code,
              static_cast<int>(ErrorCode::ConnectionNotInitialized));
}

TEST(ServerSession, PingAllowedBeforeInitialize) {
    // FR-CORE-017.
    ServerFixture f;
    f.transport->deliver(
        Message(JsonRpcRequest{RequestId(std::int64_t{6}), methods::kPing, {}}));
    auto sent = f.transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto& response = as_response(*sent);
    EXPECT_FALSE(response.is_error());
    EXPECT_EQ(*response.result, json::object());
}

TEST(ServerSession, UndeclaredCapabilityRejected) {
    // FR-CORE-007/-32002.
    ServerFixture f;  // Declares no capabilities.
    f.handshake();

    f.transport->deliver(
        Message(JsonRpcRequest{RequestId(std::int64_t{7}), "tools/list", {}}));
    auto sent = f.transport->wait_for_sent(1);
    ASSERT_TRUE(sent.has_value());
    ASSERT_TRUE(as_response(*sent).is_error());
    EXPECT_EQ(as_response(*sent).error->code,
              static_cast<int>(ErrorCode::CapabilityNotSupported));
}

TEST(ServerSession, DeclaredCapabilityPassesGate) {
    auto options = ServerFixture::default_options();
    options.capabilities.tools = ToolsCapability{true};
    ServerFixture f(std::move(options));
    f.handshake();

    // Gate passes; no handler registered yet, so the router answers -32601.
    f.transport->deliver(
        Message(JsonRpcRequest{RequestId(std::int64_t{8}), "tools/list", {}}));
    auto sent = f.transport->wait_for_sent(1);
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(as_response(*sent).error->code,
              static_cast<int>(ErrorCode::MethodNotFound));
}

TEST(ServerSession, DoubleInitializeRejected) {
    ServerFixture f;
    f.handshake();
    f.transport->deliver(make_initialize(99));
    auto sent = f.transport->wait_for_sent(1);
    ASSERT_TRUE(sent.has_value());
    ASSERT_TRUE(as_response(*sent).is_error());
    EXPECT_EQ(as_response(*sent).error->code,
              static_cast<int>(ErrorCode::InvalidRequest));
}

TEST(ServerSession, ParseErrorAnsweredWithNullId) {
    ServerFixture f;
    f.transport->deliver_error(Error(ErrorCode::ParseError, "invalid JSON"));
    auto sent = f.transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto& response = as_response(*sent);
    EXPECT_FALSE(response.id.has_value());
    EXPECT_EQ(response.error->code, static_cast<int>(ErrorCode::ParseError));
}

TEST(ServerSession, CancellationTracking) {
    // FR-CORE-015.
    ServerFixture f;
    f.transport->deliver(make_initialize(1));
    ASSERT_TRUE(f.transport->wait_for_sent(0).has_value());
    f.transport->deliver(make_initialized());

    // Cancelling an unknown/completed request is ignored gracefully.
    CancelledNotification unknown{RequestId(std::int64_t{424242}), std::nullopt};
    f.transport->deliver(Message(
        JsonRpcNotification{methods::kNotificationCancelled, unknown.to_json()}));
    EXPECT_TRUE(f.session.is_cancelled(RequestId(std::int64_t{424242})));

    // The initialize request must never be treated as cancelled.
    CancelledNotification init_cancel{RequestId(std::int64_t{1}), std::nullopt};
    f.transport->deliver(Message(JsonRpcNotification{
        methods::kNotificationCancelled, init_cancel.to_json()}));
    EXPECT_FALSE(f.session.is_cancelled(RequestId(std::int64_t{1})));
}

// --- Outgoing requests (base Session over a mock peer) ---

TEST(Session, OutgoingRequestReceivesResponse) {
    auto transport = std::make_shared<MockTransport>();
    Session session(transport);
    session.connect();

    std::promise<Result<json>> got;
    auto future = got.get_future();
    session.send_request("custom/op", json{{"x", 1}},
                         [&got](Result<json> r) { got.set_value(std::move(r)); });

    auto sent = transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto& request = std::get<JsonRpcRequest>(*sent);
    EXPECT_EQ(request.method, "custom/op");

    JsonRpcResponse response;
    response.id = request.id;
    response.result = json{{"y", 2}};
    transport->deliver(Message(response));

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().at("y"), 2);
}

TEST(Session, RequestTimeoutFailsAndCancels) {
    // FR-CORE-004.
    auto transport = std::make_shared<MockTransport>();
    Session session(transport);
    session.connect();

    std::promise<Result<json>> got;
    auto future = got.get_future();
    Session::RequestOptions options;
    options.timeout = std::chrono::milliseconds(150);
    session.send_request("slow/op", std::nullopt,
                         [&got](Result<json> r) { got.set_value(std::move(r)); },
                         options);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("timed out"), std::string::npos);

    // The timeout also emits notifications/cancelled for the request.
    auto cancel = transport->wait_for_sent(1);
    ASSERT_TRUE(cancel.has_value());
    const auto& note = std::get<JsonRpcNotification>(*cancel);
    EXPECT_EQ(note.method, methods::kNotificationCancelled);
}

TEST(Session, ProgressResetsTimeout) {
    // FR-CORE-014: progress notifications reset the timeout clock.
    auto transport = std::make_shared<MockTransport>();
    Session session(transport);
    session.connect();

    std::promise<Result<json>> got;
    auto future = got.get_future();
    std::atomic<int> progress_updates{0};

    Session::RequestOptions options;
    options.timeout = std::chrono::milliseconds(500);
    options.progress_token = ProgressToken(std::string("tok"));
    options.on_progress = [&progress_updates](const ProgressNotification&) {
        ++progress_updates;
    };
    session.send_request("slow/op", std::nullopt,
                         [&got](Result<json> r) { got.set_value(std::move(r)); },
                         options);

    auto sent = transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto& request = std::get<JsonRpcRequest>(*sent);
    EXPECT_EQ(request.params->at("_meta").at("progressToken"), "tok");

    // Halfway through the timeout, report progress.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ProgressNotification progress{ProgressToken(std::string("tok")), 0.5,
                                  1.0, std::nullopt};
    transport->deliver(Message(JsonRpcNotification{
        methods::kNotificationProgress, progress.to_json()}));

    // Past the original 500ms deadline the request must still be pending...
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(400)),
              std::future_status::timeout);
    // ...and it times out once the reset deadline expires.
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    EXPECT_FALSE(future.get());
    EXPECT_GE(progress_updates.load(), 1);
}

TEST(Session, CancelOutgoingRequest) {
    // FR-CORE-015: cancel emits the notification and ignores late responses.
    auto transport = std::make_shared<MockTransport>();
    Session session(transport);
    session.connect();

    std::promise<Result<json>> got;
    auto future = got.get_future();
    const auto id = session.send_request(
        "slow/op", std::nullopt,
        [&got](Result<json> r) { got.set_value(std::move(r)); });

    session.cancel_request(id, "user aborted");

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_FALSE(future.get());

    auto cancel = transport->wait_for_sent(1);
    ASSERT_TRUE(cancel.has_value());
    const auto& note = std::get<JsonRpcNotification>(*cancel);
    EXPECT_EQ(note.method, methods::kNotificationCancelled);
    EXPECT_EQ(note.params->at("reason"), "user aborted");

    // A late response for the cancelled id is ignored without incident.
    JsonRpcResponse late;
    late.id = id;
    late.result = json{{"late", true}};
    transport->deliver(Message(late));
}

TEST(Session, CloseFailsPendingRequests) {
    auto transport = std::make_shared<MockTransport>();
    Session session(transport);
    session.connect();

    std::promise<void> closed;
    session.set_close_callback([&closed] { closed.set_value(); });

    std::promise<Result<json>> got;
    auto future = got.get_future();
    session.send_request("never/answered", std::nullopt,
                         [&got](Result<json> r) { got.set_value(std::move(r)); });

    transport->peer_close();

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("closed"), std::string::npos);
    EXPECT_EQ(closed.get_future().wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_EQ(session.state(), SessionState::Shutdown);
}

// --- ClientSession ---

TEST(ClientSession, InitializeHandshake) {
    auto transport = std::make_shared<MockTransport>();
    ClientOptions options;
    options.client_info = Implementation{"test-client", std::nullopt, "1.0"};
    ClientSession session(transport, options);
    session.connect();

    auto result_future = std::async(std::launch::async,
                                    [&session] { return session.initialize(); });

    auto sent = transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto& request = std::get<JsonRpcRequest>(*sent);
    EXPECT_EQ(request.method, methods::kInitialize);
    EXPECT_EQ(request.params->at("protocolVersion"), kProtocolVersion);

    InitializeResult init_result{kProtocolVersion, ServerCapabilities{},
                                 Implementation{"srv", std::nullopt, "2.0"},
                                 std::nullopt};
    JsonRpcResponse response;
    response.id = request.id;
    response.result = json(init_result);
    transport->deliver(Message(response));

    auto result = result_future.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().server_info.name, "srv");
    EXPECT_EQ(session.state(), SessionState::Operating);

    // FR-CORE-010 step 3: notifications/initialized was sent.
    auto note = transport->wait_for_sent(1);
    ASSERT_TRUE(note.has_value());
    EXPECT_EQ(std::get<JsonRpcNotification>(*note).method,
              methods::kNotificationInitialized);
}

TEST(ClientSession, RejectsUnsupportedServerVersion) {
    // FR-CORE-006: incompatible version -> reject and disconnect.
    auto transport = std::make_shared<MockTransport>();
    ClientOptions options;
    options.client_info = Implementation{"test-client", std::nullopt, "1.0"};
    ClientSession session(transport, options);
    session.connect();

    auto result_future = std::async(std::launch::async,
                                    [&session] { return session.initialize(); });

    auto sent = transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const auto& request = std::get<JsonRpcRequest>(*sent);

    InitializeResult init_result{"1999-01-01", ServerCapabilities{},
                                 Implementation{"srv", std::nullopt, "2.0"},
                                 std::nullopt};
    JsonRpcResponse response;
    response.id = request.id;
    response.result = json(init_result);
    transport->deliver(Message(response));

    auto result = result_future.get();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("unsupported protocol version"),
              std::string::npos);
    EXPECT_TRUE(transport->disconnected());
}

}  // namespace
