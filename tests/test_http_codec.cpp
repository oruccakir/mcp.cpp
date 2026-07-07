#include <gtest/gtest.h>

#include "../src/transport/http/http_codec.hpp"

namespace {

using namespace mcp::detail;

TEST(HttpCodec, ParsesRequestHead) {
    const std::string raw =
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1:3001\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "{}";
    HttpHead head;
    std::size_t consumed = 0;
    std::string error;
    ASSERT_TRUE(parse_head(raw, true, head, consumed, error)) << error;
    EXPECT_EQ(head.method, "POST");
    EXPECT_EQ(head.target, "/mcp");
    EXPECT_FALSE(head.is_response());
    EXPECT_EQ(head.header("content-length"), "2");
    EXPECT_EQ(raw.substr(consumed), "{}");
}

TEST(HttpCodec, ParsesResponseHeadIncrementally) {
    const std::string raw =
        "HTTP/1.1 202 Accepted\r\nMCP-Protocol-Version: 2025-11-25\r\n\r\n";
    HttpHead head;
    std::size_t consumed = 0;
    std::string error;
    // Feed partial: not complete yet, no error.
    EXPECT_FALSE(parse_head(raw.substr(0, 20), false, head, consumed, error));
    EXPECT_TRUE(error.empty());
    ASSERT_TRUE(parse_head(raw, false, head, consumed, error)) << error;
    EXPECT_TRUE(head.is_response());
    EXPECT_EQ(head.status, 202);
    EXPECT_EQ(head.header("mcp-protocol-version"), "2025-11-25");
    EXPECT_EQ(consumed, raw.size());
}

TEST(HttpCodec, HeaderNamesAreCaseInsensitive) {
    const std::string raw =
        "GET /mcp HTTP/1.1\r\nACCEPT: Text/Event-Stream\r\n\r\n";
    HttpHead head;
    std::size_t consumed = 0;
    std::string error;
    ASSERT_TRUE(parse_head(raw, true, head, consumed, error));
    EXPECT_TRUE(head.header_contains("accept", "text/event-stream"));
}

TEST(HttpCodec, RejectsMalformedStartLines) {
    HttpHead head;
    std::size_t consumed = 0;
    std::string error;
    EXPECT_FALSE(parse_head("NOT-HTTP\r\n\r\n", true, head, consumed, error));
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(parse_head("BANANA 200 OK\r\n\r\n", false, head, consumed, error));
    EXPECT_FALSE(error.empty());
}

TEST(HttpCodec, SerializersAddContentLength) {
    const auto request = serialize_request(
        "POST", "/mcp", {{"Content-Type", "application/json"}}, "{\"a\":1}");
    EXPECT_NE(request.find("Content-Length: 7\r\n"), std::string::npos);
    EXPECT_NE(request.find("POST /mcp HTTP/1.1\r\n"), std::string::npos);

    const auto response =
        serialize_response(200, "OK", {{"Content-Type", "application/json"}}, "{}");
    EXPECT_NE(response.find("Content-Length: 2\r\n"), std::string::npos);

    // Streaming (SSE) heads must not carry Content-Length.
    const auto streaming = serialize_response(
        200, "OK", {{"Content-Type", "text/event-stream"}}, "", true);
    EXPECT_EQ(streaming.find("Content-Length"), std::string::npos);
}

TEST(HttpCodec, ChunkedDecoding) {
    ChunkedDecoder decoder;
    std::string buffer = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    std::string out;
    EXPECT_EQ(decoder.feed(buffer, out), ChunkedDecoder::Status::Done);
    EXPECT_EQ(out, "Wikipedia");

    // Split across feeds.
    ChunkedDecoder decoder2;
    std::string part = "4\r\nWi";
    std::string out2;
    EXPECT_EQ(decoder2.feed(part, out2), ChunkedDecoder::Status::NeedMore);
    part += "ki\r\n0\r\n\r\n";
    EXPECT_EQ(decoder2.feed(part, out2), ChunkedDecoder::Status::Done);
    EXPECT_EQ(out2, "Wiki");

    ChunkedDecoder decoder3;
    std::string bad = "zz\r\n";
    std::string out3;
    EXPECT_EQ(decoder3.feed(bad, out3), ChunkedDecoder::Status::Error);
}

TEST(HttpCodec, SseEventFormatting) {
    SseEvent event;
    event.id = "42";
    event.retry_ms = 3000;
    event.data = "line1\nline2";
    const auto text = format_sse_event(event);
    EXPECT_EQ(text,
              "retry: 3000\n"
              "id: 42\n"
              "data: line1\n"
              "data: line2\n"
              "\n");
}

TEST(HttpCodec, SseParserRoundTrip) {
    SseEvent event;
    event.id = "7";
    event.data = R"({"jsonrpc":"2.0","method":"x"})";
    const auto text = format_sse_event(event);

    SseParser parser;
    std::vector<SseEvent> events;
    // Feed byte by byte to exercise incremental buffering.
    for (const char c : text) {
        parser.feed(&c, 1, events);
    }
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].id, "7");
    EXPECT_EQ(events[0].data, event.data);
}

TEST(HttpCodec, SseParserIgnoresCommentsAndHandlesRetry) {
    const std::string stream =
        ": keep-alive\n"
        "retry: 250\n"
        "id: 1\n"
        "data: hello\n"
        "\n"
        "data: multi\n"
        "data: line\n"
        "\n";
    SseParser parser;
    std::vector<SseEvent> events;
    parser.feed(stream.data(), stream.size(), events);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].retry_ms, 250);
    EXPECT_EQ(events[0].data, "hello");
    EXPECT_EQ(events[1].data, "multi\nline");
    EXPECT_FALSE(events[1].id.has_value());
}

}  // namespace
