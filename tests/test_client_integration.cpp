// Phase 3 end-to-end: mcp::Client against real subprocess servers, in both
// directions (FR-TEST-002).

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <mcp/client/client.hpp>
#include <mcp/transport/stdio_client_transport.hpp>

namespace {

using namespace mcp;

std::shared_ptr<StdioClientTransport> spawn(const char* path) {
    StdioServerParameters parameters;
    parameters.command = path;
    return std::make_shared<StdioClientTransport>(parameters);
}

TEST(ClientIntegration, TypedWrappersAgainstToolbox) {
    Client client("phase3-test", "1.0.0");
    auto init = client.connect(spawn(TOOLBOX_STDIO_PATH));
    ASSERT_TRUE(init) << init.error().message;
    EXPECT_EQ(init.value().server_info.name, "toolbox-stdio");

    auto tools = client.list_tools();
    ASSERT_TRUE(tools) << tools.error().message;
    ASSERT_EQ(tools.value().items.size(), 1u);
    EXPECT_EQ(tools.value().items[0].name, "echo");

    auto call = client.call_tool("echo", json{{"message", "typed api"}});
    ASSERT_TRUE(call) << call.error().message;
    EXPECT_EQ(std::get<TextContent>(call.value().content[0]).text, "typed api");

    auto resources = client.list_resources();
    ASSERT_TRUE(resources) << resources.error().message;
    EXPECT_EQ(resources.value().items[0].uri, "mem://readme");

    auto read = client.read_resource("mem://files/x.txt");
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(read.value().contents[0].text, "file body");

    auto prompt = client.get_prompt("greet", json{{"who", "bro"}});
    ASSERT_TRUE(prompt) << prompt.error().message;
    EXPECT_EQ(std::get<TextContent>(prompt.value().messages[0].content).text,
              "Say hello to bro");

    auto completion = client.complete_prompt("greet", "who", "w");
    ASSERT_TRUE(completion) << completion.error().message;
    ASSERT_EQ(completion.value().values.size(), 1u);
    EXPECT_EQ(completion.value().values[0], "world");

    EXPECT_TRUE(client.ping());
    client.disconnect();
}

TEST(ClientIntegration, ServerInitiatedFeaturesViaProber) {
    Client client("phase3-prober-test", "1.0.0");
    ASSERT_TRUE(client.roots().add_root(Root{"file:///workspace", "Workspace"}));
    client.set_sampling_handler(
        [](const CreateMessageParams& params) -> Result<CreateMessageResult> {
            CreateMessageResult result;
            result.model = "mock-llm";
            bool saw_tool_result = false;
            for (const auto& message : params.messages) {
                if (std::holds_alternative<ToolResultContent>(message.content)) {
                    saw_tool_result = true;
                }
            }
            if (params.tools && !saw_tool_result) {
                result.stop_reason = "toolUse";
                result.content.push_back(Content(
                    ToolUseContent{"use-1", "multiply", json{{"a", 6}, {"b", 7}}}));
            } else {
                result.stop_reason = "endTurn";
                result.content.push_back(text_content("42"));
            }
            return result;
        });
    client.set_elicitation_handler(
        [](const ElicitRequest& request) -> Result<ElicitResult> {
            EXPECT_EQ(request.message, "Who is probing?");
            return ElicitResult{ElicitAction::Accept, json{{"name", "tester"}}};
        });

    auto init = client.connect(spawn(PROBER_STDIO_PATH));
    ASSERT_TRUE(init) << init.error().message;

    auto started = client.call_tool("probe");
    ASSERT_TRUE(started) << started.error().message;
    EXPECT_EQ(std::get<TextContent>(started.value().content[0]).text, "started");

    // Poll for the background probe to finish (server->client round-trips).
    json report;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto result = client.call_tool("probe_result");
        ASSERT_TRUE(result) << result.error().message;
        const auto text = std::get<TextContent>(result.value().content[0]).text;
        if (text != "pending") {
            report = json::parse(text);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_FALSE(report.is_null()) << "probe never completed";

    // roots/list answered with the registered root (FR-CLI-004).
    ASSERT_TRUE(report.contains("roots")) << report.dump();
    EXPECT_EQ(report.at("roots").at("roots")[0].at("uri"), "file:///workspace");

    // Sampling ran the full tool-use loop (FR-CLI-002): tool executed
    // server-side, final turn ended with the text answer.
    ASSERT_TRUE(report.contains("sampling")) << report.dump();
    EXPECT_EQ(report.at("sampling").at("stopReason"), "endTurn");
    EXPECT_EQ(report.at("sampling").at("content").at("text"), "42");

    // Elicitation accepted with schema-valid content (FR-CLI-006).
    ASSERT_TRUE(report.contains("elicitation")) << report.dump();
    EXPECT_EQ(report.at("elicitation").at("action"), "accept");
    EXPECT_EQ(report.at("elicitation").at("content").at("name"), "tester");

    client.disconnect();
}

}  // namespace
