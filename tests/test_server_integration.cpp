// End-to-end Phase 2: spawn the toolbox_stdio server (built on the Server
// facade) as a real subprocess and exercise tools, resources, prompts, and
// completion over stdio (FR-TEST-002).

#include <gtest/gtest.h>

#include <chrono>

#include <mcp/core/session.hpp>
#include <mcp/transport/stdio_client_transport.hpp>

namespace {

using namespace mcp;

TEST(ServerIntegration, FullFeatureRoundTrip) {
    StdioServerParameters parameters;
    parameters.command = TOOLBOX_STDIO_PATH;

    auto transport = std::make_shared<StdioClientTransport>(parameters);
    ClientOptions options;
    options.client_info = Implementation{"phase2-test", std::nullopt, "1.0"};
    options.initialize_timeout = std::chrono::milliseconds(5000);
    ClientSession session(transport, options);
    session.connect();

    auto init = session.initialize();
    ASSERT_TRUE(init) << init.error().message;
    EXPECT_EQ(init.value().server_info.name, "toolbox-stdio");

    // Capabilities were auto-derived from the populated registries.
    ASSERT_TRUE(session.server_capabilities().has_value());
    EXPECT_TRUE(session.server_capabilities()->tools.has_value());
    EXPECT_TRUE(session.server_capabilities()->resources.has_value());
    EXPECT_TRUE(session.server_capabilities()->prompts.has_value());
    EXPECT_TRUE(session.server_capabilities()->completions.has_value());

    Session::RequestOptions request_options;
    request_options.timeout = std::chrono::milliseconds(5000);

    // tools/list -> tools/call
    auto tools = session.send_request_sync("tools/list", json::object(),
                                           request_options);
    ASSERT_TRUE(tools) << tools.error().message;
    ASSERT_EQ(tools.value().at("tools").size(), 1u);
    EXPECT_EQ(tools.value().at("tools")[0].at("name"), "echo");

    auto call = session.send_request_sync(
        "tools/call",
        json{{"name", "echo"}, {"arguments", {{"message", "over the wire"}}}},
        request_options);
    ASSERT_TRUE(call) << call.error().message;
    EXPECT_EQ(call.value().at("content")[0].at("text"), "over the wire");

    // resources/read: static resource and template match.
    auto readme = session.send_request_sync(
        "resources/read", json{{"uri", "mem://readme"}}, request_options);
    ASSERT_TRUE(readme) << readme.error().message;
    EXPECT_EQ(readme.value().at("contents")[0].at("text"), "toolbox readme");

    auto templated = session.send_request_sync(
        "resources/read", json{{"uri", "mem://files/a.txt"}}, request_options);
    ASSERT_TRUE(templated) << templated.error().message;
    EXPECT_EQ(templated.value().at("contents")[0].at("text"), "file body");

    // prompts/get with required argument.
    auto prompt = session.send_request_sync(
        "prompts/get", json{{"name", "greet"}, {"arguments", {{"who", "bro"}}}},
        request_options);
    ASSERT_TRUE(prompt) << prompt.error().message;
    EXPECT_EQ(prompt.value().at("messages")[0].at("content").at("text"),
              "Say hello to bro");

    // completion/complete against the prompt argument.
    auto completion = session.send_request_sync(
        "completion/complete",
        json{{"ref", {{"type", "ref/prompt"}, {"name", "greet"}}},
             {"argument", {{"name", "who"}, {"value", "w"}}}},
        request_options);
    ASSERT_TRUE(completion) << completion.error().message;
    EXPECT_EQ(completion.value().at("completion").at("values")[0], "world");

    session.disconnect();
}

}  // namespace
