#include <gtest/gtest.h>

#include <mcp/server/prompt_provider.hpp>

namespace {

using namespace mcp;

Prompt make_prompt(const std::string& name, bool required_arg = false) {
    Prompt p;
    p.name = name;
    p.description = "test prompt";
    if (required_arg) {
        PromptArgument arg;
        arg.name = "topic";
        arg.required = true;
        p.arguments = std::vector<PromptArgument>{arg};
    }
    return p;
}

PromptHandler simple_handler() {
    return [](const json& args) -> Result<GetPromptResult> {
        GetPromptResult result;
        result.description = "generated";
        result.messages.push_back(PromptMessage{
            Role::User,
            text_content("write about " + args.value("topic", "anything"))});
        return result;
    };
}

TEST(PromptProvider, AddListGet) {
    PromptProvider provider;
    ASSERT_TRUE(provider.add_prompt(make_prompt("blog"), simple_handler()));
    EXPECT_FALSE(provider.add_prompt(make_prompt("blog"), simple_handler()));
    EXPECT_FALSE(provider.add_prompt(make_prompt(""), simple_handler()));
    EXPECT_FALSE(provider.add_prompt(make_prompt("nohandler"), nullptr));

    auto page = provider.list_prompts(std::nullopt);
    ASSERT_TRUE(page);
    ASSERT_EQ(page.value().items.size(), 1u);
    EXPECT_EQ(page.value().items[0].name, "blog");

    auto result = provider.get_prompt("blog", json{{"topic", "mcp"}});
    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().messages.size(), 1u);

    // FR-SRV-016: wire shape with role + typed content.
    const auto j = result.value().to_json();
    EXPECT_EQ(j.at("messages")[0].at("role"), "user");
    EXPECT_EQ(j.at("messages")[0].at("content").at("type"), "text");
    EXPECT_EQ(j.at("messages")[0].at("content").at("text"), "write about mcp");
}

TEST(PromptProvider, RequiredArgumentsValidated) {
    // FR-SRV-015
    PromptProvider provider;
    ASSERT_TRUE(provider.add_prompt(make_prompt("blog", true), simple_handler()));

    auto missing = provider.get_prompt("blog", json::object());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, static_cast<int>(ErrorCode::InvalidParams));
    EXPECT_NE(missing.error().message.find("topic"), std::string::npos);

    EXPECT_TRUE(provider.get_prompt("blog", json{{"topic", "x"}}));

    auto unknown = provider.get_prompt("nope", json::object());
    ASSERT_FALSE(unknown);
}

TEST(PromptProvider, ArgumentSerializesPerSpec) {
    Prompt p = make_prompt("blog", true);
    const json j = p;
    EXPECT_EQ(j.at("arguments")[0].at("name"), "topic");
    EXPECT_TRUE(j.at("arguments")[0].at("required").get<bool>());
    EXPECT_FALSE(j.at("arguments")[0].contains("description"));
}

TEST(PromptProvider, ChangedCallbackAndCompletions) {
    PromptProvider provider;
    int changes = 0;
    provider.set_changed_callback([&changes] { ++changes; });
    ASSERT_TRUE(provider.add_prompt(make_prompt("a"), simple_handler()));
    ASSERT_TRUE(provider.remove_prompt("a"));
    EXPECT_EQ(changes, 2);

    provider.set_completion("blog", "topic", [](const std::string&) {
        CompleteResult r;
        r.values = {"ai", "mcp"};
        r.total = 2;
        return r;
    });
    auto completion = provider.complete("blog", "topic", "");
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->values.size(), 2u);
    EXPECT_TRUE(provider.has_completions());

    const auto j = completion->to_json();
    EXPECT_EQ(j.at("completion").at("values").size(), 2u);
    EXPECT_EQ(j.at("completion").at("total"), 2);
}

}  // namespace
