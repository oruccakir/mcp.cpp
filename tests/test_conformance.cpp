// Spec-conformance suite (SRS §5.11, §10): drives every server-handled
// method and notification from the SRS §10 message catalog through a fully
// featured Server as raw wire JSON, and asserts response *shape* — exact
// method strings, camelCase keys, jsonrpc/id envelope rules, error codes.
// No official MCP conformance suite exists; this is the in-house one.

#include <gtest/gtest.h>

#include <future>
#include <memory>

#include <mcp/client/client.hpp>
#include <mcp/methods.hpp>
#include <mcp/server/server.hpp>

#include "mock_transport.hpp"

namespace {

using namespace mcp;
using mcp_test::MockTransport;

/// Fully featured server (every capability populated) over MockTransport.
/// Requests are injected as raw wire text through parse_frame, exactly as a
/// transport would deliver them.
struct Conformance {
    Server server{"conformance-server", "9.9.9"};
    std::shared_ptr<MockTransport> transport;
    std::unique_ptr<ServerSession> session;
    std::size_t sent_index = 0;
    std::int64_t next_id = 500;

    Conformance() {
        ToolSpec echo;
        echo.description = "echo";
        echo.input_schema = json{{"type", "object"}};
        echo.handler = [](const json& args) -> CallToolResult {
            return {{text_content(args.value("message", "hi"))}};
        };
        EXPECT_TRUE(server.register_tool("echo", std::move(echo)));

        Resource doc;
        doc.uri = "mem://doc";
        doc.name = "Doc";
        EXPECT_TRUE(server.resources().add_resource(
            doc, [](const std::string& uri) -> Result<ReadResourceResult> {
                ResourceContents contents;
                contents.uri = uri;
                contents.text = "body";
                return ReadResourceResult{{contents}};
            }));
        ResourceTemplate tmpl;
        tmpl.uri_template = "mem://files/{name}";
        tmpl.name = "Files";
        EXPECT_TRUE(server.resources().add_resource_template(tmpl));
        server.resources().set_read_handler(
            [](const std::string& uri) -> Result<ReadResourceResult> {
                ResourceContents contents;
                contents.uri = uri;
                contents.text = "templated";
                return ReadResourceResult{{contents}};
            });

        Prompt greet;
        greet.name = "greet";
        PromptArgument who;
        who.name = "who";
        who.required = true;
        greet.arguments = std::vector<PromptArgument>{who};
        EXPECT_TRUE(server.prompts().add_prompt(
            greet, [](const json&) -> Result<GetPromptResult> {
                GetPromptResult result;
                result.messages.push_back(
                    PromptMessage{Role::User, text_content("hello")});
                return result;
            }));
        server.prompts().set_completion(
            "greet", "who", [](const std::string&) {
                CompleteResult r;
                r.values = {"world"};
                return r;
            });

        transport = std::make_shared<MockTransport>();
        session = std::make_unique<ServerSession>(transport,
                                                  server.server_options());
        server.attach(*session);
        session->connect();
    }

    /// Delivers raw wire text exactly as a transport would.
    void deliver_wire(const std::string& text) {
        auto frame = parse_frame(text);
        ASSERT_TRUE(frame) << text;
        for (auto& message : frame.value().messages) {
            transport->deliver(std::move(message));
        }
    }

    json request_wire(const std::string& method, const std::string& params_json) {
        const auto id = next_id++;
        std::string wire = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                           R"(,"method":")" + method + "\"";
        if (!params_json.empty()) {
            wire += R"(,"params":)" + params_json;
        }
        wire += "}";
        deliver_wire(wire);

        auto sent = transport->wait_for_sent(sent_index++);
        EXPECT_TRUE(sent.has_value()) << method;
        const json envelope = message_to_json(*sent);

        // Envelope conformance for every response (JSON-RPC 2.0 / FR-CORE-001).
        EXPECT_EQ(envelope.at("jsonrpc"), "2.0") << method;
        EXPECT_EQ(envelope.at("id"), id) << method;
        EXPECT_NE(envelope.contains("result"), envelope.contains("error"))
            << method << ": result XOR error";
        return envelope;
    }

    json expect_result(const std::string& method,
                       const std::string& params_json = "") {
        const json envelope = request_wire(method, params_json);
        EXPECT_TRUE(envelope.contains("result"))
            << method << " -> " << envelope.dump();
        return envelope.value("result", json::object());
    }

    int expect_error(const std::string& method,
                     const std::string& params_json = "") {
        const json envelope = request_wire(method, params_json);
        EXPECT_TRUE(envelope.contains("error"))
            << method << " -> " << envelope.dump();
        return envelope.at("error").value("code", 0);
    }

    void handshake() {
        const json result = expect_result(
            methods::kInitialize,
            R"({"protocolVersion":"2025-11-25","capabilities":{},)"
            R"("clientInfo":{"name":"conformance","version":"1"}})");
        EXPECT_EQ(result.at("protocolVersion"), kProtocolVersion);
        deliver_wire(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        EXPECT_EQ(session->state(), SessionState::Operating);
    }

    JsonRpcNotification next_notification() {
        auto sent = transport->wait_for_sent(sent_index++);
        EXPECT_TRUE(sent.has_value());
        return std::get<JsonRpcNotification>(*sent);
    }
};

// --- Lifecycle (SRS §10: initialize / initialized / ping) ------------------

TEST(Conformance, InitializeResultShape) {
    Conformance c;
    const json result = c.expect_result(
        "initialize",
        R"({"protocolVersion":"2025-11-25","capabilities":{},)"
        R"("clientInfo":{"name":"x","version":"1"}})");
    EXPECT_EQ(result.at("protocolVersion"), "2025-11-25");
    ASSERT_TRUE(result.contains("capabilities"));
    ASSERT_TRUE(result.contains("serverInfo"));
    EXPECT_TRUE(result.at("serverInfo").contains("name"));
    EXPECT_TRUE(result.at("serverInfo").contains("version"));
    // Derived capabilities use spec keys (camelCase).
    EXPECT_TRUE(result.at("capabilities").at("tools").contains("listChanged"));
    EXPECT_TRUE(result.at("capabilities").at("resources").contains("subscribe"));
}

TEST(Conformance, PingReturnsEmptyObject) {
    Conformance c;
    EXPECT_EQ(c.expect_result("ping"), json::object());  // FR-CORE-017
}

// --- Tools ------------------------------------------------------------------

TEST(Conformance, ToolsCatalog) {
    Conformance c;
    c.handshake();

    const json list = c.expect_result(methods::kToolsList);
    ASSERT_TRUE(list.at("tools").is_array());
    const json& tool = list.at("tools").at(0);
    EXPECT_TRUE(tool.contains("name"));
    EXPECT_TRUE(tool.contains("inputSchema"));  // camelCase on the wire
    EXPECT_FALSE(tool.contains("input_schema"));

    const json call = c.expect_result(
        methods::kToolsCall, R"({"name":"echo","arguments":{"message":"m"}})");
    ASSERT_TRUE(call.at("content").is_array());
    EXPECT_EQ(call.at("content").at(0).at("type"), "text");
    EXPECT_TRUE(call.contains("isError"));

    EXPECT_EQ(c.expect_error(methods::kToolsCall, R"({"name":"missing"})"),
              static_cast<int>(ErrorCode::InvalidParams));
}

// --- Resources ---------------------------------------------------------------

TEST(Conformance, ResourcesCatalog) {
    Conformance c;
    c.handshake();

    const json list = c.expect_result(methods::kResourcesList);
    const json& resource = list.at("resources").at(0);
    EXPECT_TRUE(resource.contains("uri"));
    EXPECT_TRUE(resource.contains("name"));

    const json templates = c.expect_result(methods::kResourcesTemplatesList);
    EXPECT_TRUE(templates.at("resourceTemplates").at(0).contains("uriTemplate"));

    const json read =
        c.expect_result(methods::kResourcesRead, R"({"uri":"mem://doc"})");
    ASSERT_TRUE(read.at("contents").is_array());
    EXPECT_EQ(read.at("contents").at(0).at("uri"), "mem://doc");

    EXPECT_EQ(c.expect_error(methods::kResourcesRead, R"({"uri":"mem://nope"})"),
              static_cast<int>(ErrorCode::ResourceNotFound));  // -32003
    EXPECT_EQ(c.expect_error(methods::kResourcesRead, R"({"uri":"not a uri"})"),
              static_cast<int>(ErrorCode::InvalidUri));  // -32005

    EXPECT_EQ(c.expect_result(methods::kResourcesSubscribe,
                              R"({"uri":"mem://doc"})"),
              json::object());
    EXPECT_EQ(c.expect_result(methods::kResourcesUnsubscribe,
                              R"({"uri":"mem://doc"})"),
              json::object());
}

// --- Prompts & completion ----------------------------------------------------

TEST(Conformance, PromptsCatalog) {
    Conformance c;
    c.handshake();

    const json list = c.expect_result(methods::kPromptsList);
    const json& prompt = list.at("prompts").at(0);
    EXPECT_EQ(prompt.at("name"), "greet");
    EXPECT_TRUE(prompt.at("arguments").at(0).contains("required"));

    const json get = c.expect_result(
        methods::kPromptsGet, R"({"name":"greet","arguments":{"who":"w"}})");
    ASSERT_TRUE(get.at("messages").is_array());
    EXPECT_EQ(get.at("messages").at(0).at("role"), "user");
    EXPECT_TRUE(get.at("messages").at(0).at("content").contains("type"));

    // Missing required argument -> -32602.
    EXPECT_EQ(c.expect_error(methods::kPromptsGet, R"({"name":"greet"})"),
              static_cast<int>(ErrorCode::InvalidParams));
}

TEST(Conformance, CompletionCatalog) {
    Conformance c;
    c.handshake();
    const json result = c.expect_result(
        methods::kCompletionComplete,
        R"({"ref":{"type":"ref/prompt","name":"greet"},)"
        R"("argument":{"name":"who","value":"w"}})");
    ASSERT_TRUE(result.at("completion").at("values").is_array());
    EXPECT_EQ(result.at("completion").at("values").at(0), "world");
}

// --- Logging -----------------------------------------------------------------

TEST(Conformance, LoggingSetLevelAndMessageShape) {
    Conformance c;
    c.handshake();
    EXPECT_EQ(c.expect_result(methods::kLoggingSetLevel, R"({"level":"debug"})"),
              json::object());

    c.server.logger().info(json{{"k", "v"}});
    const auto note = c.next_notification();
    EXPECT_EQ(note.method, "notifications/message");  // exact spec string
    EXPECT_EQ(note.params->at("level"), "info");
    EXPECT_TRUE(note.params->contains("data"));
}

// --- Notification method strings (SRS §10) ----------------------------------

TEST(Conformance, ListChangedNotificationStrings) {
    Conformance c;
    c.handshake();

    ToolSpec extra;
    extra.input_schema = json{{"type", "object"}};
    extra.handler = [](const json&) { return CallToolResult{}; };
    ASSERT_TRUE(c.server.register_tool("extra", std::move(extra)));
    EXPECT_EQ(c.next_notification().method, "notifications/tools/list_changed");

    Resource r2;
    r2.uri = "mem://doc2";
    r2.name = "Doc2";
    ASSERT_TRUE(c.server.resources().add_resource(
        r2, [](const std::string& uri) -> Result<ReadResourceResult> {
            return ReadResourceResult{{ResourceContents{uri, {}, "x", {}}}};
        }));
    EXPECT_EQ(c.next_notification().method,
              "notifications/resources/list_changed");

    Prompt p2;
    p2.name = "second";
    ASSERT_TRUE(c.server.prompts().add_prompt(
        p2, [](const json&) -> Result<GetPromptResult> {
            return GetPromptResult{};
        }));
    EXPECT_EQ(c.next_notification().method,
              "notifications/prompts/list_changed");
}

TEST(Conformance, ResourceUpdatedNotificationShape) {
    Conformance c;
    c.handshake();
    ASSERT_EQ(c.expect_result(methods::kResourcesSubscribe,
                              R"({"uri":"mem://doc"})"),
              json::object());
    c.server.notify_resource_updated("mem://doc");
    const auto note = c.next_notification();
    EXPECT_EQ(note.method, "notifications/resources/updated");
    EXPECT_EQ(note.params->at("uri"), "mem://doc");
}

// --- Error catalog (FR-CORE-002/003) ------------------------------------------

TEST(Conformance, ErrorCatalog) {
    Conformance c;
    c.handshake();

    // Unknown method -> -32601 exactly.
    EXPECT_EQ(c.expect_error("no/such/method"), -32601);

    // Batch: response array, one entry per request, ids echoed (FR-CORE-001).
    c.deliver_wire(
        R"([{"jsonrpc":"2.0","id":900,"method":"ping"},)"
        R"({"jsonrpc":"2.0","id":901,"method":"ping"}])");
    const json first = message_to_json(*c.transport->wait_for_sent(c.sent_index++));
    const json second = message_to_json(*c.transport->wait_for_sent(c.sent_index++));
    EXPECT_EQ(first.at("id"), 900);
    EXPECT_EQ(second.at("id"), 901);
}

TEST(Conformance, GatingErrorCodes) {
    // Before initialize: -32000; undeclared capability: -32002 (FR-CORE-005/7).
    Conformance c;
    EXPECT_EQ(c.expect_error(methods::kToolsList), -32000);

    Server bare("bare", "1.0");
    auto transport = std::make_shared<MockTransport>();
    ServerSession session(transport, bare.server_options());
    bare.attach(session);
    session.connect();
    transport->deliver(Message(JsonRpcRequest{
        RequestId(std::int64_t{1}), methods::kInitialize,
        json::parse(R"({"protocolVersion":"2025-11-25","capabilities":{},)"
                    R"("clientInfo":{"name":"x","version":"1"}})")}));
    ASSERT_TRUE(transport->wait_for_sent(0).has_value());
    transport->deliver(
        Message(JsonRpcNotification{methods::kNotificationInitialized, {}}));
    transport->deliver(Message(
        JsonRpcRequest{RequestId(std::int64_t{2}), methods::kToolsList, {}}));
    const json envelope = message_to_json(*transport->wait_for_sent(1));
    EXPECT_EQ(envelope.at("error").at("code"), -32002);
}

// --- Client-side catalog (sampling / roots / elicitation) ---------------------

TEST(Conformance, ClientSideCatalog) {
    Client client("conformance-client", "1.0");
    ASSERT_TRUE(client.roots().add_root(Root{"file:///ws", "WS"}));
    client.set_sampling_handler(
        [](const CreateMessageParams&) -> Result<CreateMessageResult> {
            CreateMessageResult result;
            result.model = "m";
            result.stop_reason = "endTurn";
            result.content.push_back(text_content("ok"));
            return result;
        });
    client.set_elicitation_handler(
        [](const ElicitRequest&) -> Result<ElicitResult> {
            return ElicitResult{ElicitAction::Accept, json{{"a", 1}}};
        });

    auto transport = std::make_shared<MockTransport>();
    auto init_future = std::async(std::launch::async,
                                  [&] { return client.connect(transport); });
    auto sent = transport->wait_for_sent(0);
    ASSERT_TRUE(sent.has_value());
    const json init_envelope = message_to_json(*sent);
    // Client initialize params shape (SRS §10).
    EXPECT_EQ(init_envelope.at("params").at("protocolVersion"), kProtocolVersion);
    EXPECT_TRUE(init_envelope.at("params").contains("clientInfo"));
    EXPECT_TRUE(
        init_envelope.at("params").at("capabilities").contains("sampling"));

    JsonRpcResponse response;
    response.id = std::get<JsonRpcRequest>(*sent).id;
    response.result = json(InitializeResult{
        kProtocolVersion, ServerCapabilities{},
        Implementation{"srv", std::nullopt, "1"}, std::nullopt});
    transport->deliver(Message(response));
    ASSERT_TRUE(init_future.get());
    ASSERT_TRUE(transport->wait_for_sent(1).has_value());  // initialized

    // roots/list result shape.
    transport->deliver(Message(JsonRpcRequest{RequestId(std::int64_t{10}),
                                              methods::kRootsList, {}}));
    const json roots = message_to_json(*transport->wait_for_sent(2));
    EXPECT_EQ(roots.at("result").at("roots").at(0).at("uri"), "file:///ws");

    // sampling/createMessage result shape.
    CreateMessageParams params;
    params.messages.push_back(SamplingMessage{Role::User, text_content("q")});
    transport->deliver(Message(JsonRpcRequest{
        RequestId(std::int64_t{11}), methods::kSamplingCreateMessage,
        json(params)}));
    const json sampled = message_to_json(*transport->wait_for_sent(3));
    EXPECT_EQ(sampled.at("result").at("role"), "assistant");
    EXPECT_TRUE(sampled.at("result").contains("model"));
    EXPECT_TRUE(sampled.at("result").contains("content"));

    // elicitation/create result shape.
    transport->deliver(Message(JsonRpcRequest{
        RequestId(std::int64_t{12}), methods::kElicitationCreate,
        json{{"message", "?"}}}));
    const json elicited = message_to_json(*transport->wait_for_sent(4));
    EXPECT_EQ(elicited.at("result").at("action"), "accept");

    client.disconnect();
}

}  // namespace
