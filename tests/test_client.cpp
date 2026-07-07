#include <gtest/gtest.h>

#include <future>

#include <mcp/client/client.hpp>
#include <mcp/methods.hpp>

#include "mock_transport.hpp"

namespace {

using namespace mcp;
using mcp_test::MockTransport;

struct ClientFixture {
    Client client{"test-host", "1.0.0"};
    std::shared_ptr<MockTransport> transport = std::make_shared<MockTransport>();
    std::size_t sent_index = 0;

    /// Completes connect(): answers initialize and skips the initialized
    /// notification (FR-CORE-010).
    void finish_handshake() {
        auto sent = transport->wait_for_sent(sent_index++);
        ASSERT_TRUE(sent.has_value());
        const auto& request = std::get<JsonRpcRequest>(*sent);
        ASSERT_EQ(request.method, methods::kInitialize);
        InitializeResult result{kProtocolVersion, ServerCapabilities{},
                                Implementation{"srv", std::nullopt, "1"},
                                std::nullopt};
        JsonRpcResponse response;
        response.id = request.id;
        response.result = json(result);
        transport->deliver(Message(response));
        sent_index++;  // notifications/initialized
    }

    void connect() {
        auto future = std::async(std::launch::async,
                                 [this] { return client.connect(transport); });
        finish_handshake();
        auto init = future.get();
        ASSERT_TRUE(init) << init.error().message;
    }

    /// Runs a blocking client call and answers the request it produces.
    template <typename Fn>
    auto answered(Fn&& fn, json result) -> decltype(fn()) {
        auto future = std::async(std::launch::async, std::forward<Fn>(fn));
        auto sent = transport->wait_for_sent(sent_index++);
        EXPECT_TRUE(sent.has_value());
        const auto& request = std::get<JsonRpcRequest>(*sent);
        JsonRpcResponse response;
        response.id = request.id;
        response.result = std::move(result);
        transport->deliver(Message(response));
        return future.get();
    }

    JsonRpcResponse server_request(const std::string& method, json params) {
        transport->deliver(Message(
            JsonRpcRequest{RequestId(std::int64_t{7000 + static_cast<int>(sent_index)}),
                           method, std::move(params)}));
        auto sent = transport->wait_for_sent(sent_index++);
        EXPECT_TRUE(sent.has_value());
        return std::get<JsonRpcResponse>(*sent);
    }
};

TEST(Client, CapabilitiesDerivedFromConfiguration) {
    // FR-CORE-009
    Client bare("bare", "1.0");
    const json bare_caps = bare.capabilities();
    EXPECT_FALSE(bare_caps.contains("roots"));
    EXPECT_FALSE(bare_caps.contains("sampling"));
    EXPECT_FALSE(bare_caps.contains("elicitation"));

    Client full("full", "1.0");
    ASSERT_TRUE(full.roots().add_root(Root{"file:///p", std::nullopt}));
    full.set_sampling_handler(
        [](const CreateMessageParams&) -> Result<CreateMessageResult> {
            return CreateMessageResult{};
        });
    full.set_elicitation_handler(
        [](const ElicitRequest&) -> Result<ElicitResult> {
            return ElicitResult{};
        });
    const json caps = full.capabilities();
    EXPECT_TRUE(caps.at("roots").at("listChanged").get<bool>());
    EXPECT_TRUE(caps.at("sampling").at("tools").get<bool>());
    EXPECT_TRUE(caps.at("elicitation").at("form").get<bool>());
}

TEST(Client, ConnectPerformsHandshake) {
    ClientFixture f;
    f.connect();
    EXPECT_EQ(f.client.session()->state(), SessionState::Operating);
    ASSERT_TRUE(f.client.server_capabilities().has_value());
}

TEST(Client, TypedWrappers) {
    ClientFixture f;
    f.connect();

    auto tools = f.answered(
        [&] { return f.client.list_tools(); },
        json{{"tools", json::array({json{{"name", "echo"},
                                         {"inputSchema", {{"type", "object"}}}}})},
             {"nextCursor", "5"}});
    ASSERT_TRUE(tools) << tools.error().message;
    ASSERT_EQ(tools.value().items.size(), 1u);
    EXPECT_EQ(tools.value().items[0].name, "echo");
    EXPECT_EQ(tools.value().next_cursor, "5");

    auto call = f.answered(
        [&] { return f.client.call_tool("echo", json{{"message", "hi"}}); },
        json{{"content", json::array({json{{"type", "text"}, {"text", "hi"}}})},
             {"isError", false}});
    ASSERT_TRUE(call) << call.error().message;
    EXPECT_FALSE(call.value().is_error);
    EXPECT_EQ(std::get<TextContent>(call.value().content[0]).text, "hi");

    auto read = f.answered(
        [&] { return f.client.read_resource("mem://a"); },
        json{{"contents",
              json::array({json{{"uri", "mem://a"}, {"text", "body"}}})}});
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(read.value().contents[0].text, "body");

    auto prompt = f.answered(
        [&] { return f.client.get_prompt("greet", json{{"who", "x"}}); },
        json{{"messages",
              json::array({json{
                  {"role", "user"},
                  {"content", json{{"type", "text"}, {"text", "hello x"}}}}})}});
    ASSERT_TRUE(prompt) << prompt.error().message;
    ASSERT_EQ(prompt.value().messages.size(), 1u);
    EXPECT_EQ(std::get<TextContent>(prompt.value().messages[0].content).text,
              "hello x");

    auto completion = f.answered(
        [&] { return f.client.complete_prompt("greet", "who", "b"); },
        json{{"completion",
              {{"values", json::array({"bro"})}, {"hasMore", false}}}});
    ASSERT_TRUE(completion) << completion.error().message;
    EXPECT_EQ(completion.value().values[0], "bro");

    auto level = f.answered(
        [&] { return f.client.set_logging_level(LoggingLevel::Warning); },
        json::object());
    EXPECT_TRUE(level);

    // Server error propagates as the wrapper's error.
    auto failing = std::async(std::launch::async,
                              [&] { return f.client.call_tool("nope"); });
    auto sent = f.transport->wait_for_sent(f.sent_index++);
    ASSERT_TRUE(sent.has_value());
    JsonRpcResponse err_response;
    err_response.id = std::get<JsonRpcRequest>(*sent).id;
    err_response.error = Error(ErrorCode::InvalidParams, "unknown tool: nope");
    f.transport->deliver(Message(err_response));
    auto failed = failing.get();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, static_cast<int>(ErrorCode::InvalidParams));
}

TEST(Client, AnswersRootsListRequests) {
    // FR-CLI-004
    ClientFixture f;
    ASSERT_TRUE(f.client.roots().add_root(Root{"file:///proj", "Project"}));
    f.connect();

    auto response = f.server_request(methods::kRootsList, json::object());
    ASSERT_FALSE(response.is_error());
    ASSERT_EQ(response.result->at("roots").size(), 1u);
    EXPECT_EQ(response.result->at("roots")[0].at("uri"), "file:///proj");
}

TEST(Client, EmitsRootsListChanged) {
    // FR-CLI-005
    ClientFixture f;
    f.client.enable_roots();
    f.connect();

    ASSERT_TRUE(f.client.roots().add_root(Root{"file:///new", std::nullopt}));
    auto sent = f.transport->wait_for_sent(f.sent_index++);
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(std::get<JsonRpcNotification>(*sent).method,
              methods::kNotificationRootsListChanged);
}

TEST(Client, AnswersSamplingRequests) {
    // FR-CLI-001
    ClientFixture f;
    f.client.set_sampling_handler(
        [](const CreateMessageParams& params) -> Result<CreateMessageResult> {
            CreateMessageResult result;
            result.model = "mock-llm";
            result.stop_reason = "endTurn";
            const auto& first =
                std::get<TextContent>(params.messages.at(0).content);
            result.content.push_back(text_content("echo: " + first.text));
            return result;
        });
    f.connect();

    CreateMessageParams params;
    params.messages.push_back(SamplingMessage{Role::User, text_content("hi")});
    auto response =
        f.server_request(methods::kSamplingCreateMessage, json(params));
    ASSERT_FALSE(response.is_error());
    EXPECT_EQ(response.result->at("model"), "mock-llm");
    EXPECT_EQ(response.result->at("content").at("text"), "echo: hi");
}

TEST(Client, UndeclaredSamplingRejected) {
    // FR-CORE-007: no handler -> capability not declared -> -32002.
    ClientFixture f;
    f.connect();
    auto response =
        f.server_request(methods::kSamplingCreateMessage, json::object());
    ASSERT_TRUE(response.is_error());
    EXPECT_EQ(response.error->code,
              static_cast<int>(ErrorCode::CapabilityNotSupported));
}

TEST(Client, ElicitationAcceptValidatesSchema) {
    // FR-CLI-006
    ClientFixture f;
    json answer{{"name", "Oruc"}};
    f.client.set_elicitation_handler(
        [&answer](const ElicitRequest& request) -> Result<ElicitResult> {
            EXPECT_EQ(request.message, "Who are you?");
            return ElicitResult{ElicitAction::Accept, answer};
        });
    f.connect();

    const json schema{{"type", "object"},
                      {"properties", {{"name", {{"type", "string"}}}}},
                      {"required", json::array({"name"})}};
    auto ok = f.server_request(
        methods::kElicitationCreate,
        json{{"message", "Who are you?"}, {"requestedSchema", schema}});
    ASSERT_FALSE(ok.is_error());
    EXPECT_EQ(ok.result->at("action"), "accept");
    EXPECT_EQ(ok.result->at("content").at("name"), "Oruc");

    // Content violating the schema is rejected client-side.
    answer = json{{"name", 42}};
    auto bad = f.server_request(
        methods::kElicitationCreate,
        json{{"message", "Who are you?"}, {"requestedSchema", schema}});
    ASSERT_TRUE(bad.is_error());
    EXPECT_EQ(bad.error->code, static_cast<int>(ErrorCode::InvalidParams));
}

TEST(Client, NotificationCallbacks) {
    ClientFixture f;
    int tools_changes = 0;
    std::string updated_uri;
    json last_log;
    f.client.on_tools_list_changed([&tools_changes] { ++tools_changes; });
    f.client.on_resource_updated(
        [&updated_uri](const std::string& uri) { updated_uri = uri; });
    f.client.on_log_message([&last_log](const json& params) { last_log = params; });
    f.connect();

    f.transport->deliver(Message(
        JsonRpcNotification{methods::kNotificationToolsListChanged, {}}));
    f.transport->deliver(Message(JsonRpcNotification{
        methods::kNotificationResourcesUpdated, json{{"uri", "mem://a"}}}));
    f.transport->deliver(Message(JsonRpcNotification{
        methods::kNotificationMessage,
        json{{"level", "info"}, {"data", "hello"}}}));

    EXPECT_EQ(tools_changes, 1);
    EXPECT_EQ(updated_uri, "mem://a");
    EXPECT_EQ(last_log.at("level"), "info");
}

}  // namespace
