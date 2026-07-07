#include <gtest/gtest.h>

#include <mcp/detail/uri_template.hpp>
#include <mcp/server/resource_provider.hpp>

namespace {

using namespace mcp;

Resource make_resource(const std::string& uri, const std::string& name) {
    Resource r;
    r.uri = uri;
    r.name = name;
    return r;
}

ReadResourceResult text_result(const std::string& uri, const std::string& body) {
    ResourceContents contents;
    contents.uri = uri;
    contents.mime_type = "text/plain";
    contents.text = body;
    return ReadResourceResult{{contents}};
}

TEST(UriTemplate, Level1Matching) {
    // FR-SRV-010 subset.
    auto m = detail::match_uri_template("file:///{path}", "file:///a/b/c.txt");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->at("path"), "a/b/c.txt");

    auto multi = detail::match_uri_template("db://{table}/{id}", "db://users/42");
    ASSERT_TRUE(multi.has_value());
    EXPECT_EQ(multi->at("table"), "users");
    EXPECT_EQ(multi->at("id"), "42");

    EXPECT_FALSE(detail::match_uri_template("file:///{path}", "http://x"));
    EXPECT_FALSE(detail::match_uri_template("file:///{path}", "file:///"));
    EXPECT_FALSE(detail::match_uri_template("a://{x}b{y}", "a://b"));  // empty var
    EXPECT_FALSE(detail::match_uri_template("a://{unclosed", "a://x"));
}

TEST(ResourceProvider, AddListRead) {
    ResourceProvider provider;
    ASSERT_TRUE(provider.add_resource(
        make_resource("mem://a", "A"),
        [](const std::string& uri) { return text_result(uri, "contents-a"); }));
    ASSERT_FALSE(provider.add_resource(make_resource("mem://a", "dup")));
    ASSERT_FALSE(provider.add_resource(make_resource("not a uri", "bad")));

    auto page = provider.list_resources(std::nullopt);
    ASSERT_TRUE(page);
    ASSERT_EQ(page.value().items.size(), 1u);
    EXPECT_EQ(page.value().items[0].name, "A");

    auto read = provider.read_resource("mem://a");
    ASSERT_TRUE(read);
    EXPECT_EQ(read.value().contents[0].text, "contents-a");

    const auto j = read.value().to_json();
    EXPECT_EQ(j.at("contents")[0].at("uri"), "mem://a");
    EXPECT_FALSE(j.at("contents")[0].contains("blob"));
}

TEST(ResourceProvider, ErrorCodes) {
    ResourceProvider provider;
    // FR-CORE-003: -32003 unknown, -32005 invalid.
    auto not_found = provider.read_resource("mem://missing");
    ASSERT_FALSE(not_found);
    EXPECT_EQ(not_found.error().code,
              static_cast<int>(ErrorCode::ResourceNotFound));

    auto invalid = provider.read_resource("no-scheme");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, static_cast<int>(ErrorCode::InvalidUri));
}

TEST(ResourceProvider, TemplateFallbackRead) {
    // SRS §6.3 pattern: template + provider-level read handler.
    ResourceProvider provider;
    ResourceTemplate tmpl;
    tmpl.uri_template = "file:///{path}";
    tmpl.name = "Project Files";
    ASSERT_TRUE(provider.add_resource_template(tmpl));
    provider.set_read_handler([](const std::string& uri) {
        return text_result(uri, "from-template");
    });

    auto read = provider.read_resource("file:///src/main.cpp");
    ASSERT_TRUE(read);
    EXPECT_EQ(read.value().contents[0].text, "from-template");

    auto templates = provider.list_resource_templates(std::nullopt);
    ASSERT_TRUE(templates);
    EXPECT_EQ(templates.value().items[0].uri_template, "file:///{path}");

    // Non-matching URI still 404s.
    EXPECT_FALSE(provider.read_resource("db://users/1"));
}

TEST(ResourceProvider, ReadHandlerFailuresBecomeErrors) {
    ResourceProvider provider;
    ASSERT_TRUE(provider.add_resource(
        make_resource("mem://boom", "Boom"),
        [](const std::string&) -> Result<ReadResourceResult> {
            throw McpError(Error(ErrorCode::ResourceNotFound, "vanished"));
        }));
    auto read = provider.read_resource("mem://boom");
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, static_cast<int>(ErrorCode::ResourceNotFound));
}

TEST(ResourceProvider, Subscriptions) {
    // FR-SRV-012
    ResourceProvider provider;
    ASSERT_TRUE(provider.add_resource(
        make_resource("mem://a", "A"),
        [](const std::string& uri) { return text_result(uri, "x"); }));

    EXPECT_TRUE(provider.subscribe("mem://a"));
    EXPECT_TRUE(provider.is_subscribed("mem://a"));

    auto unknown = provider.subscribe("mem://nope");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code,
              static_cast<int>(ErrorCode::ResourceNotFound));

    EXPECT_TRUE(provider.unsubscribe("mem://a"));
    EXPECT_FALSE(provider.is_subscribed("mem://a"));
    EXPECT_FALSE(provider.unsubscribe("mem://a"));

    // Removing a resource drops its subscription.
    EXPECT_TRUE(provider.subscribe("mem://a"));
    EXPECT_TRUE(provider.remove_resource("mem://a"));
    EXPECT_FALSE(provider.is_subscribed("mem://a"));
}

TEST(ResourceProvider, CompletionCallbacks) {
    ResourceProvider provider;
    provider.set_completion("file:///{path}", "path", [](const std::string& v) {
        CompleteResult r;
        if (std::string("src").rfind(v, 0) == 0) {
            r.values.push_back("src");
        }
        return r;
    });

    auto hit = provider.complete("file:///{path}", "path", "s");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->values.size(), 1u);

    EXPECT_FALSE(provider.complete("file:///{path}", "other", "s").has_value());
    EXPECT_TRUE(provider.has_completions());
}

}  // namespace
