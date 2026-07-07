#include <gtest/gtest.h>

#include <mcp/server/tool_registry.hpp>

namespace {

using namespace mcp;

Tool make_tool(const std::string& name) {
    Tool tool;
    tool.name = name;
    tool.description = "test tool";
    return tool;
}

ToolHandler ok_handler(const std::string& reply) {
    return [reply](const json&) { return CallToolResult{{text_content(reply)}}; };
}

TEST(ToolNames, ValidationRules) {
    // FR-SRV-003
    EXPECT_TRUE(is_valid_tool_name("echo"));
    EXPECT_TRUE(is_valid_tool_name("ns.tool_name-v2"));
    EXPECT_FALSE(is_valid_tool_name(""));
    EXPECT_FALSE(is_valid_tool_name("has space"));
    EXPECT_FALSE(is_valid_tool_name("comma,"));
    EXPECT_FALSE(is_valid_tool_name(std::string(129, 'a')));
    EXPECT_TRUE(is_valid_tool_name(std::string(128, 'a')));
}

TEST(ToolRegistry, RegisterRejectsInvalidAndDuplicate) {
    ToolRegistry registry;
    EXPECT_TRUE(registry.register_tool(make_tool("echo"), ok_handler("hi")));
    EXPECT_FALSE(registry.register_tool(make_tool("echo"), ok_handler("hi")));
    EXPECT_FALSE(registry.register_tool(make_tool("bad name"), ok_handler("x")));
    EXPECT_FALSE(registry.register_tool(make_tool("nohandler"), nullptr));
    EXPECT_EQ(registry.size(), 1u);
}

TEST(ToolRegistry, SerializesPerSpec) {
    // FR-SRV-002: camelCase keys, omit-if-absent optionals.
    Tool tool = make_tool("echo");
    tool.input_schema = json{{"type", "object"}};
    const json j = tool;
    EXPECT_EQ(j.at("name"), "echo");
    EXPECT_TRUE(j.contains("inputSchema"));
    EXPECT_FALSE(j.contains("outputSchema"));
    EXPECT_FALSE(j.contains("title"));
}

TEST(ToolRegistry, ListPaginates) {
    // FR-SRV-004
    ToolRegistry registry;
    registry.set_page_size(2);
    for (const auto* name : {"a", "b", "c", "d", "e"}) {
        ASSERT_TRUE(registry.register_tool(make_tool(name), ok_handler(name)));
    }

    auto page1 = registry.list_tools(std::nullopt);
    ASSERT_TRUE(page1);
    EXPECT_EQ(page1.value().items.size(), 2u);
    ASSERT_TRUE(page1.value().next_cursor.has_value());

    auto page2 = registry.list_tools(page1.value().next_cursor);
    ASSERT_TRUE(page2);
    EXPECT_EQ(page2.value().items[0].name, "c");

    auto page3 = registry.list_tools(page2.value().next_cursor);
    ASSERT_TRUE(page3);
    EXPECT_EQ(page3.value().items.size(), 1u);
    EXPECT_FALSE(page3.value().next_cursor.has_value());

    // FR-CORE-003: invalid cursors -> -32006.
    auto bad = registry.list_tools(std::string("not-a-cursor"));
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, static_cast<int>(ErrorCode::PaginationError));
    auto out_of_range = registry.list_tools(std::string("999"));
    ASSERT_FALSE(out_of_range);
}

TEST(ToolRegistry, CallValidatesArguments) {
    ToolRegistry registry;
    Tool tool = make_tool("greet");
    tool.input_schema =
        json{{"type", "object"},
             {"properties", {{"name", {{"type", "string"}}}}},
             {"required", json::array({"name"})}};
    ASSERT_TRUE(registry.register_tool(tool, [](const json& args) {
        return CallToolResult{
            {text_content("hello " + args.at("name").get<std::string>())}};
    }));

    auto ok = registry.call_tool("greet", json{{"name", "bro"}});
    ASSERT_TRUE(ok);
    EXPECT_FALSE(ok.value().is_error);
    EXPECT_EQ(std::get<TextContent>(ok.value().content[0]).text, "hello bro");

    // Missing required arg -> protocol error, handler not invoked.
    auto missing = registry.call_tool("greet", json::object());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, static_cast<int>(ErrorCode::InvalidParams));

    auto unknown = registry.call_tool("nope", json::object());
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, static_cast<int>(ErrorCode::InvalidParams));
}

TEST(ToolRegistry, HandlerFailuresAreResultsNotProtocolErrors) {
    // FR-SRV-005: execution errors -> isError=true.
    ToolRegistry registry;
    ASSERT_TRUE(registry.register_tool(
        make_tool("throws"), [](const json&) -> CallToolResult {
            throw std::runtime_error("tool blew up");
        }));

    auto result = registry.call_tool("throws", json::object());
    ASSERT_TRUE(result);  // No protocol error...
    EXPECT_TRUE(result.value().is_error);
    EXPECT_EQ(std::get<TextContent>(result.value().content[0]).text,
              "tool blew up");

    const auto j = result.value().to_json();
    EXPECT_TRUE(j.at("isError").get<bool>());
}

TEST(ToolRegistry, ChangedCallbackFires) {
    // FR-SRV-006
    ToolRegistry registry;
    int changes = 0;
    registry.set_changed_callback([&changes] { ++changes; });

    ASSERT_TRUE(registry.register_tool(make_tool("a"), ok_handler("a")));
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(registry.remove_tool("a"));
    EXPECT_EQ(changes, 2);
    EXPECT_FALSE(registry.remove_tool("a"));
    EXPECT_EQ(changes, 2);  // No-op removals do not notify.
}

}  // namespace
