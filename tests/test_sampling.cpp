#include <gtest/gtest.h>

#include <atomic>
#include <future>

#include <mcp/client/sampling.hpp>
#include <mcp/methods.hpp>

#include "mock_transport.hpp"

namespace {

using namespace mcp;
using mcp_test::MockTransport;

TEST(SamplingTypes, CreateMessageParamsRoundTrip) {
    // FR-CLI-001/003: camelCase wire keys, omit-if-absent.
    CreateMessageParams params;
    params.messages.push_back(SamplingMessage{Role::User, text_content("hi")});
    params.system_prompt = "be brief";
    params.max_tokens = 64;
    params.model_preferences = ModelPreferences{
        std::vector<ModelHint>{{"claude"}}, std::nullopt, 0.2, 0.9};

    const json j = params;
    EXPECT_EQ(j.at("maxTokens"), 64);
    EXPECT_EQ(j.at("systemPrompt"), "be brief");
    EXPECT_EQ(j.at("messages")[0].at("role"), "user");
    EXPECT_EQ(j.at("modelPreferences").at("intelligencePriority"), 0.9);
    EXPECT_FALSE(j.at("modelPreferences").contains("costPriority"));
    EXPECT_FALSE(j.contains("temperature"));

    const auto round = j.get<CreateMessageParams>();
    EXPECT_EQ(round.messages.size(), 1u);
    EXPECT_EQ(round.model_preferences->speed_priority, 0.2);
    EXPECT_EQ(std::get<TextContent>(round.messages[0].content).text, "hi");
}

TEST(SamplingTypes, CreateMessageResultShapes) {
    CreateMessageResult result;
    result.model = "test-model";
    result.stop_reason = "endTurn";
    result.content.push_back(text_content("answer"));

    // Single block serializes as an object, not an array.
    const auto j = result.to_json();
    EXPECT_TRUE(j.at("content").is_object());
    EXPECT_EQ(j.at("content").at("text"), "answer");

    auto parsed = CreateMessageResult::from_json(j);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().model, "test-model");
    EXPECT_EQ(parsed.value().stop_reason, "endTurn");

    // Multi-block (tool use) serializes as an array.
    result.content.push_back(
        Content(ToolUseContent{"u1", "add", json{{"a", 1}}}));
    const auto multi = result.to_json();
    EXPECT_TRUE(multi.at("content").is_array());
    auto parsed_multi = CreateMessageResult::from_json(multi);
    ASSERT_TRUE(parsed_multi);
    EXPECT_EQ(parsed_multi.value().content.size(), 2u);

    EXPECT_FALSE(CreateMessageResult::from_json(json{{"role", "assistant"}}));
}

TEST(SamplingTypes, ToolResultContentRoundTrip) {
    ToolResultContent tool_result{"use-1", {text_content("42")}, false};
    const auto j = content_to_json(Content(tool_result));
    EXPECT_EQ(j.at("type"), "tool_result");
    EXPECT_EQ(j.at("toolUseId"), "use-1");

    auto parsed = content_from_json(j);
    ASSERT_TRUE(parsed);
    const auto& round = std::get<ToolResultContent>(parsed.value());
    EXPECT_EQ(round.tool_use_id, "use-1");
    ASSERT_EQ(round.content.size(), 1u);
    EXPECT_EQ(std::get<TextContent>(round.content[0]).text, "42");
}

// --- Tool-use loop (FR-CLI-002) over a mock peer ---

CreateMessageParams loop_params() {
    CreateMessageParams params;
    params.messages.push_back(
        SamplingMessage{Role::User, text_content("compute 40+2")});
    params.max_tokens = 32;
    params.tools = json::array({json{{"name", "add"}}});
    return params;
}

json tool_use_response(const std::string& id) {
    CreateMessageResult result;
    result.model = "fake-llm";
    result.stop_reason = "toolUse";
    result.content.push_back(Content(ToolUseContent{id, "add", json{{"a", 40}}}));
    return result.to_json();
}

json end_turn_response(const std::string& text) {
    CreateMessageResult result;
    result.model = "fake-llm";
    result.stop_reason = "endTurn";
    result.content.push_back(text_content(text));
    return result.to_json();
}

void answer(MockTransport& transport, std::size_t index, const json& result) {
    auto sent = transport.wait_for_sent(index);
    ASSERT_TRUE(sent.has_value());
    const auto& request = std::get<JsonRpcRequest>(*sent);
    ASSERT_EQ(request.method, methods::kSamplingCreateMessage);
    JsonRpcResponse response;
    response.id = request.id;
    response.result = result;
    transport.deliver(Message(response));
}

TEST(SamplingLoop, ExecutesToolsUntilEndTurn) {
    auto transport = std::make_shared<MockTransport>();
    Session session(transport);
    session.connect();

    std::atomic<int> executions{0};
    auto loop = std::async(std::launch::async, [&] {
        return run_sampling_tool_loop(
            session, loop_params(),
            [&executions](const ToolUseContent& use) {
                ++executions;
                EXPECT_EQ(use.name, "add");
                return ToolResultContent{use.id, {text_content("42")}, false};
            },
            4, std::chrono::milliseconds(2000));
    });

    answer(*transport, 0, tool_use_response("use-1"));

    // Second request must carry the accumulated tool_use + tool_result turns.
    auto second = transport->wait_for_sent(1);
    ASSERT_TRUE(second.has_value());
    const auto& request = std::get<JsonRpcRequest>(*second);
    const auto& messages = request.params->at("messages");
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[1].at("content").at("type"), "tool_use");
    EXPECT_EQ(messages[2].at("content").at("type"), "tool_result");
    EXPECT_EQ(messages[2].at("content").at("content")[0].at("text"), "42");

    JsonRpcResponse response;
    response.id = request.id;
    response.result = end_turn_response("the answer is 42");
    transport->deliver(Message(response));

    auto result = loop.get();
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().stop_reason, "endTurn");
    EXPECT_EQ(executions.load(), 1);
}

TEST(SamplingLoop, StopsAfterMaxTurns) {
    auto transport = std::make_shared<MockTransport>();
    Session session(transport);
    session.connect();

    auto loop = std::async(std::launch::async, [&] {
        return run_sampling_tool_loop(
            session, loop_params(),
            [](const ToolUseContent& use) {
                return ToolResultContent{use.id, {text_content("x")}, false};
            },
            2, std::chrono::milliseconds(2000));
    });

    answer(*transport, 0, tool_use_response("u1"));
    answer(*transport, 1, tool_use_response("u2"));

    auto result = loop.get();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("max turns"), std::string::npos);
}

}  // namespace
