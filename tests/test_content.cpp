#include <gtest/gtest.h>

#include <mcp/content.hpp>

namespace {

using namespace mcp;

TEST(Content, TextRoundTrip) {
    // FR-SER-002
    const auto j = content_to_json(text_content("hello"));
    EXPECT_EQ(j.at("type"), "text");
    EXPECT_EQ(j.at("text"), "hello");

    auto parsed = content_from_json(j);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::get<TextContent>(parsed.value()).text, "hello");
}

TEST(Content, ImageAndAudioRoundTrip) {
    const auto img = content_to_json(image_content("aGk=", "image/png"));
    EXPECT_EQ(img.at("type"), "image");
    EXPECT_EQ(img.at("mimeType"), "image/png");
    ASSERT_TRUE(content_from_json(img));

    const auto aud = content_to_json(audio_content("aGk=", "audio/wav"));
    EXPECT_EQ(aud.at("type"), "audio");
    auto parsed = content_from_json(aud);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::get<AudioContent>(parsed.value()).data, "aGk=");
}

TEST(Content, EmbeddedResourceRoundTrip) {
    EmbeddedResource res;
    res.resource.uri = "file:///a.txt";
    res.resource.mime_type = "text/plain";
    res.resource.text = "body";
    const auto j = content_to_json(Content(res));
    EXPECT_EQ(j.at("type"), "resource");
    EXPECT_EQ(j.at("resource").at("uri"), "file:///a.txt");
    EXPECT_FALSE(j.at("resource").contains("blob"));  // omit-if-absent

    auto parsed = content_from_json(j);
    ASSERT_TRUE(parsed);
    const auto& round = std::get<EmbeddedResource>(parsed.value());
    EXPECT_EQ(round.resource.text, "body");
}

TEST(Content, ToolUseRoundTrip) {
    ToolUseContent use{"call-1", "search", json{{"q", "mcp"}}};
    const auto j = content_to_json(Content(use));
    EXPECT_EQ(j.at("type"), "tool_use");

    auto parsed = content_from_json(j);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::get<ToolUseContent>(parsed.value()).input.at("q"), "mcp");
}

TEST(Content, RejectsUnknownOrMalformed) {
    EXPECT_FALSE(content_from_json(json{{"type", "video"}}));
    EXPECT_FALSE(content_from_json(json{{"text", "no type"}}));
    EXPECT_FALSE(content_from_json(json{{"type", "image"}}));  // missing data
    EXPECT_FALSE(content_from_json(json::array()));
}

TEST(Content, ListRoundTrip) {
    std::vector<Content> contents{text_content("a"), text_content("b")};
    const auto j = content_list_to_json(contents);
    ASSERT_EQ(j.size(), 2u);

    auto parsed = content_list_from_json(j);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().size(), 2u);

    EXPECT_FALSE(content_list_from_json(json{{"not", "array"}}));
}

TEST(Content, AnnotationsSerialized) {
    TextContent text{"hi", Annotations{std::vector<std::string>{"user"}, 0.5,
                                       std::nullopt}};
    const auto j = content_to_json(Content(text));
    EXPECT_EQ(j.at("annotations").at("priority"), 0.5);
    EXPECT_FALSE(j.at("annotations").contains("lastModified"));
}

}  // namespace
