#include <gtest/gtest.h>

#include <mcp/methods.hpp>
#include <mcp/server/server.hpp>

#include "mock_transport.hpp"

namespace {

using namespace mcp;
using mcp_test::MockTransport;

ReadResourceResult text_result(const std::string& uri, const std::string& body) {
    ResourceContents contents;
    contents.uri = uri;
    contents.text = body;
    return ReadResourceResult{{contents}};
}

/// Server + attached session over a mock transport, with a request helper.
struct ServerFixture {
    Server server{"test-server", "1.0.0"};
    std::shared_ptr<MockTransport> transport;
    std::unique_ptr<ServerSession> session;
    std::int64_t next_id = 100;
    std::size_t sent_index = 0;

    void start() {
        transport = std::make_shared<MockTransport>();
        session = std::make_unique<ServerSession>(transport,
                                                  server.server_options());
        server.attach(*session);
        session->connect();

        InitializeParams params{kProtocolVersion, ClientCapabilities{},
                                Implementation{"client", std::nullopt, "1"}};
        transport->deliver(Message(JsonRpcRequest{
            RequestId(std::int64_t{1}), methods::kInitialize, json(params)}));
        ASSERT_TRUE(transport->wait_for_sent(sent_index++).has_value());
        transport->deliver(
            Message(JsonRpcNotification{methods::kNotificationInitialized, {}}));
        ASSERT_EQ(session->state(), SessionState::Operating);
    }

    JsonRpcResponse request(const std::string& method,
                            std::optional<json> params = std::nullopt) {
        transport->deliver(
            Message(JsonRpcRequest{RequestId(next_id++), method, std::move(params)}));
        auto sent = transport->wait_for_sent(sent_index++);
        EXPECT_TRUE(sent.has_value()) << method;
        return std::get<JsonRpcResponse>(*sent);
    }

    JsonRpcNotification next_notification() {
        auto sent = transport->wait_for_sent(sent_index++);
        EXPECT_TRUE(sent.has_value());
        return std::get<JsonRpcNotification>(*sent);
    }
};

void register_echo(Server& server) {
    ASSERT_TRUE(server.register_tool(
        "echo", ToolSpec{std::nullopt,
                         "Echoes back the input",
                         json{{"type", "object"},
                              {"properties",
                               {{"message", {{"type", "string"}}}}},
                              {"required", json::array({"message"})}},
                         std::nullopt,
                         [](const json& args) {
                             return CallToolResult{{text_content(
                                 args.at("message").get<std::string>())}};
                         }}));
}

TEST(Server, CapabilitiesDerivedFromRegistries) {
    // FR-CORE-008: only populated features are declared.
    Server bare("bare", "1.0");
    const json bare_caps = bare.capabilities();
    EXPECT_FALSE(bare_caps.contains("tools"));
    EXPECT_FALSE(bare_caps.contains("resources"));
    EXPECT_FALSE(bare_caps.contains("prompts"));
    EXPECT_FALSE(bare_caps.contains("completions"));
    EXPECT_TRUE(bare_caps.contains("logging"));

    Server full("full", "1.0");
    register_echo(full);
    ASSERT_TRUE(full.resources().add_resource(
        Resource{"mem://a", "A", {}, {}, {}, {}, {}},
        [](const std::string& uri) { return text_result(uri, "x"); }));
    full.prompts().set_completion("p", "arg",
                                  [](const std::string&) { return CompleteResult{}; });
    const json caps = full.capabilities();
    EXPECT_TRUE(caps.at("tools").at("listChanged").get<bool>());
    EXPECT_TRUE(caps.at("resources").at("subscribe").get<bool>());
    EXPECT_TRUE(caps.contains("completions"));
}

TEST(Server, ToolsListAndCall) {
    ServerFixture f;
    register_echo(f.server);
    f.start();

    auto list = f.request(methods::kToolsList, json::object());
    ASSERT_FALSE(list.is_error());
    ASSERT_EQ(list.result->at("tools").size(), 1u);
    EXPECT_EQ(list.result->at("tools")[0].at("name"), "echo");
    EXPECT_FALSE(list.result->contains("nextCursor"));

    auto call = f.request(
        methods::kToolsCall,
        json{{"name", "echo"}, {"arguments", {{"message", "hi bro"}}}});
    ASSERT_FALSE(call.is_error());
    EXPECT_FALSE(call.result->at("isError").get<bool>());
    EXPECT_EQ(call.result->at("content")[0].at("text"), "hi bro");

    // Schema-invalid arguments -> protocol error (FR-SRV / tools spec).
    auto bad = f.request(methods::kToolsCall,
                         json{{"name", "echo"}, {"arguments", json::object()}});
    ASSERT_TRUE(bad.is_error());
    EXPECT_EQ(bad.error->code, static_cast<int>(ErrorCode::InvalidParams));

    // Pagination errors surface as -32006 (FR-CORE-003).
    auto bad_cursor =
        f.request(methods::kToolsList, json{{"cursor", "bogus"}});
    ASSERT_TRUE(bad_cursor.is_error());
    EXPECT_EQ(bad_cursor.error->code,
              static_cast<int>(ErrorCode::PaginationError));
}

TEST(Server, UndeclaredFeatureGatedBySession) {
    // FR-CORE-007: no tools registered -> tools/* rejected with -32002.
    ServerFixture f;
    f.start();
    auto response = f.request(methods::kToolsList, json::object());
    ASSERT_TRUE(response.is_error());
    EXPECT_EQ(response.error->code,
              static_cast<int>(ErrorCode::CapabilityNotSupported));
}

TEST(Server, ResourcesEndToEnd) {
    ServerFixture f;
    ASSERT_TRUE(f.server.resources().add_resource(
        Resource{"mem://doc", "Doc", {}, {}, {}, {}, {}},
        [](const std::string& uri) { return text_result(uri, "body"); }));
    ResourceTemplate tmpl;
    tmpl.uri_template = "mem://files/{name}";
    tmpl.name = "Files";
    ASSERT_TRUE(f.server.resources().add_resource_template(tmpl));
    f.server.resources().set_read_handler(
        [](const std::string& uri) { return text_result(uri, "template"); });
    f.start();

    auto list = f.request(methods::kResourcesList, json::object());
    ASSERT_FALSE(list.is_error());
    EXPECT_EQ(list.result->at("resources")[0].at("uri"), "mem://doc");

    auto templates = f.request(methods::kResourcesTemplatesList, json::object());
    ASSERT_FALSE(templates.is_error());
    EXPECT_EQ(templates.result->at("resourceTemplates")[0].at("uriTemplate"),
              "mem://files/{name}");

    auto read = f.request(methods::kResourcesRead, json{{"uri", "mem://doc"}});
    ASSERT_FALSE(read.is_error());
    EXPECT_EQ(read.result->at("contents")[0].at("text"), "body");

    auto via_template =
        f.request(methods::kResourcesRead, json{{"uri", "mem://files/a.txt"}});
    ASSERT_FALSE(via_template.is_error());

    auto missing =
        f.request(methods::kResourcesRead, json{{"uri", "mem://nope"}});
    ASSERT_TRUE(missing.is_error());
    EXPECT_EQ(missing.error->code,
              static_cast<int>(ErrorCode::ResourceNotFound));

    // Subscribe -> updated notification -> unsubscribe -> silence.
    auto sub = f.request(methods::kResourcesSubscribe, json{{"uri", "mem://doc"}});
    ASSERT_FALSE(sub.is_error());
    f.server.notify_resource_updated("mem://doc");
    auto note = f.next_notification();
    EXPECT_EQ(note.method, methods::kNotificationResourcesUpdated);
    EXPECT_EQ(note.params->at("uri"), "mem://doc");

    auto unsub =
        f.request(methods::kResourcesUnsubscribe, json{{"uri", "mem://doc"}});
    ASSERT_FALSE(unsub.is_error());
    f.server.notify_resource_updated("mem://doc");  // No notification now.
    EXPECT_EQ(f.transport->sent_count(), f.sent_index);
}

TEST(Server, PromptsEndToEnd) {
    ServerFixture f;
    Prompt prompt;
    prompt.name = "greet";
    PromptArgument arg;
    arg.name = "who";
    arg.required = true;
    prompt.arguments = std::vector<PromptArgument>{arg};
    ASSERT_TRUE(f.server.prompts().add_prompt(
        prompt, [](const json& args) -> Result<GetPromptResult> {
            GetPromptResult r;
            r.messages.push_back(PromptMessage{
                Role::User,
                text_content("greet " + args.at("who").get<std::string>())});
            return r;
        }));
    f.start();

    auto list = f.request(methods::kPromptsList, json::object());
    ASSERT_FALSE(list.is_error());
    EXPECT_EQ(list.result->at("prompts")[0].at("name"), "greet");

    auto get = f.request(methods::kPromptsGet,
                         json{{"name", "greet"}, {"arguments", {{"who", "bro"}}}});
    ASSERT_FALSE(get.is_error());
    EXPECT_EQ(get.result->at("messages")[0].at("content").at("text"),
              "greet bro");

    auto missing = f.request(methods::kPromptsGet, json{{"name", "greet"}});
    ASSERT_TRUE(missing.is_error());
    EXPECT_EQ(missing.error->code, static_cast<int>(ErrorCode::InvalidParams));
}

TEST(Server, LoggingThresholdAndSetLevel) {
    // FR-SRV-019..021
    ServerFixture f;
    f.start();

    f.server.logger().debug(json{{"msg", "hidden"}});  // Below default Info.
    f.server.logger().warning(json{{"msg", "visible"}});
    auto note = f.next_notification();
    EXPECT_EQ(note.method, methods::kNotificationMessage);
    EXPECT_EQ(note.params->at("level"), "warning");
    EXPECT_EQ(note.params->at("data").at("msg"), "visible");

    auto set = f.request(methods::kLoggingSetLevel, json{{"level", "error"}});
    ASSERT_FALSE(set.is_error());
    f.server.logger().warning(json{{"msg", "now hidden"}});
    f.server.logger().error(json{{"msg", "shown"}});
    auto second = f.next_notification();
    EXPECT_EQ(second.params->at("level"), "error");

    auto bad = f.request(methods::kLoggingSetLevel, json{{"level", "loud"}});
    ASSERT_TRUE(bad.is_error());
}

TEST(Server, CompletionEndToEnd) {
    // FR-SRV-022/023
    ServerFixture f;
    f.server.prompts().set_completion(
        "greet", "who", [](const std::string& partial) {
            CompleteResult r;
            for (const auto* candidate : {"bro", "bella", "chief"}) {
                if (std::string(candidate).rfind(partial, 0) == 0) {
                    r.values.push_back(candidate);
                }
            }
            r.total = static_cast<int>(r.values.size());
            return r;
        });
    f.start();

    auto complete = f.request(
        methods::kCompletionComplete,
        json{{"ref", {{"type", "ref/prompt"}, {"name", "greet"}}},
             {"argument", {{"name", "who"}, {"value", "b"}}}});
    ASSERT_FALSE(complete.is_error());
    EXPECT_EQ(complete.result->at("completion").at("values").size(), 2u);

    // Unknown ref target -> empty completion, not an error.
    auto unknown = f.request(
        methods::kCompletionComplete,
        json{{"ref", {{"type", "ref/prompt"}, {"name", "nope"}}},
             {"argument", {{"name", "who"}, {"value", "b"}}}});
    ASSERT_FALSE(unknown.is_error());
    EXPECT_TRUE(unknown.result->at("completion").at("values").empty());

    auto malformed = f.request(methods::kCompletionComplete, json::object());
    ASSERT_TRUE(malformed.is_error());
}

TEST(Server, ListChangedNotifications) {
    // FR-SRV-006: mutations after the handshake notify the client.
    ServerFixture f;
    register_echo(f.server);
    f.start();

    ASSERT_TRUE(f.server.register_tool(
        "second", ToolSpec{std::nullopt, std::nullopt, json{{"type", "object"}},
                           std::nullopt,
                           [](const json&) { return CallToolResult{}; }}));
    auto note = f.next_notification();
    EXPECT_EQ(note.method, methods::kNotificationToolsListChanged);

    EXPECT_TRUE(f.server.tools().remove_tool("second"));
    EXPECT_EQ(f.next_notification().method,
              methods::kNotificationToolsListChanged);
}

}  // namespace
