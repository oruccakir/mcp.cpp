// Unit tests for the internal HTTP/1.1 + SSE codec (FR-TRAN-005..009).
// These exercise the parsers without any sockets; the transport tests in
// test_http_transport.cpp cover the socket round-trips.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <unistd.h>

#include "../src/transport/http/http_codec.hpp"
#include "../src/transport/http/socket_util.hpp"

namespace {

using mcp::detail::http::header_get;
using mcp::detail::http::HttpRequest;
using mcp::detail::http::HttpRequestParser;
using mcp::detail::http::HttpResponse;
using mcp::detail::http::HttpResponseParser;
using mcp::detail::http::ParseStatus;
using mcp::detail::http::SseEvent;
using mcp::detail::http::SseParser;
using mcp::detail::http::format_sse_event;
using mcp::detail::http::serialize_request;
using mcp::detail::http::serialize_response;

ParseStatus feed_all(HttpRequestParser& p, std::string_view s) {
    return p.feed(s);
}
ParseStatus feed_all(HttpResponseParser& p, std::string_view s) {
    return p.feed(s);
}

TEST(HttpCodec, RequestRoundTrip) {
    const std::string raw =
        "POST /mcp HTTP/1.1\r\n"
        "Host: localhost:3001\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    HttpRequestParser p;
    EXPECT_EQ(feed_all(p, raw), ParseStatus::Done);
    auto r = p.take();
    EXPECT_EQ(r.method, "POST");
    EXPECT_EQ(r.target, "/mcp");
    EXPECT_TRUE(r.keep_alive);
    ASSERT_NE(header_get(r.headers, "content-type"), nullptr);
    EXPECT_EQ(*header_get(r.headers, "content-type"), "application/json");
    EXPECT_EQ(r.body, "hello");
}

TEST(HttpCodec, RequestHeaderCaseInsensitive) {
    const std::string raw =
        "GET /mcp HTTP/1.1\r\n"
        "AcCePt: text/event-stream\r\n"
        "\r\n";
    HttpRequestParser p;
    EXPECT_EQ(feed_all(p, raw), ParseStatus::Done);
    auto r = p.take();
    ASSERT_NE(header_get(r.headers, "ACCEPT"), nullptr);
    EXPECT_EQ(*header_get(r.headers, "ACCEPT"), "text/event-stream");
    EXPECT_TRUE(r.body.empty());
}

TEST(HttpCodec, RequestBodyAcrossSplitReads) {
    HttpRequestParser p;
    EXPECT_EQ(p.feed("POST /mcp HTTP/1.1\r\nContent-Length: 10\r\n\r\n"),
              ParseStatus::NeedMore);
    EXPECT_EQ(p.feed("hello"), ParseStatus::NeedMore);
    EXPECT_EQ(p.feed("world"), ParseStatus::Done);
    EXPECT_EQ(p.take().body, "helloworld");
}

TEST(HttpCodec, RequestHttp10ConnectionClose) {
    const std::string raw =
        "POST /mcp HTTP/1.0\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    HttpRequestParser p;
    ASSERT_EQ(feed_all(p, raw), ParseStatus::Done);
    EXPECT_TRUE(p.take().keep_alive);

    HttpRequestParser p2;
    ASSERT_EQ(feed_all(p2,
                       "POST /mcp HTTP/1.1\r\nConnection: close\r\n\r\n"),
              ParseStatus::Done);
    EXPECT_FALSE(p2.take().keep_alive);
}

TEST(HttpCodec, RequestMalformed) {
    HttpRequestParser p;
    EXPECT_EQ(feed_all(p, "not a request line\r\n\r\n"), ParseStatus::Error);
}

TEST(HttpCodec, ResponseRoundTrip) {
    HttpResponse res{200, "OK", {{"content-type", "application/json"},
                                 {"content-length", "5"}},
                     "hello"};
    const std::string raw = serialize_response(res);
    HttpResponseParser p;
    ASSERT_EQ(feed_all(p, raw), ParseStatus::Done);
    auto r = p.take();
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.reason, "OK");
    EXPECT_EQ(r.body, "hello");
    ASSERT_NE(header_get(r.headers, "content-type"), nullptr);
    EXPECT_EQ(*header_get(r.headers, "content-type"), "application/json");
}

TEST(HttpCodec, ResponseChunkedBody) {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "5\r\nworld\r\n"
        "0\r\n\r\n";
    HttpResponseParser p;
    ASSERT_EQ(feed_all(p, raw), ParseStatus::Done);
    EXPECT_EQ(p.take().body, "helloworld");
}

TEST(HttpCodec, ResponseChunkedAcrossSplits) {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "3\r\nabc\r\n"
        "0\r\n\r\n";
    HttpResponseParser p;
    for (char c : raw) {
        p.feed(std::string_view(&c, 1));
    }
    ASSERT_EQ(p.feed(""), ParseStatus::Done);
    EXPECT_EQ(p.take().body, "abc");
}

TEST(HttpCodec, ResponseErrorStatus) {
    const std::string raw =
        "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    HttpResponseParser p;
    ASSERT_EQ(feed_all(p, raw), ParseStatus::Done);
    auto r = p.take();
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(r.reason, "Forbidden");
    EXPECT_TRUE(r.body.empty());
}

TEST(HttpCodec, SerializeRequest) {
    HttpRequest req{"POST", "/mcp",
                    {{"host", "localhost:3001"}, {"content-length", "2"}},
                    "{}",
                    true};
    const std::string raw = serialize_request(req);
    EXPECT_NE(raw.find("POST /mcp HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(raw.find("host: localhost:3001\r\n"), std::string::npos);
    EXPECT_NE(raw.find("\r\n\r\n{}"), std::string::npos);
}

TEST(SseCodec, EventFraming) {
    auto frame = format_sse_event(std::optional<std::string>{"7"},
                                  R"({"jsonrpc":"2.0"})", std::nullopt);
    EXPECT_EQ(frame, "id:7\ndata:{\"jsonrpc\":\"2.0\"}\n\n");

    auto empty = format_sse_event(std::optional<std::string>{"1"}, "", std::nullopt);
    EXPECT_EQ(empty, "id:1\ndata:\n\n");

    auto retry = format_sse_event(std::nullopt, "x", std::optional<std::int64_t>{3000});
    EXPECT_EQ(retry, "retry:3000\ndata:x\n\n");
}

TEST(SseCodec, MultilineData) {
    auto frame = format_sse_event(std::nullopt, "line1\nline2", std::nullopt);
    EXPECT_EQ(frame, "data:line1\ndata:line2\n\n");
}

TEST(SseCodec, ParseEvents) {
    const std::string stream =
        "id:5\ndata:{\"a\":1}\n\n"
        "data:{\"b\":2}\n\n";
    SseParser p;
    auto events = p.feed(stream);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].id.value(), "5");
    EXPECT_EQ(events[0].data, "{\"a\":1}");
    EXPECT_FALSE(events[1].id.has_value());
    EXPECT_EQ(events[1].data, "{\"b\":2}");
    EXPECT_EQ(p.last_event_id(), "5");
}

TEST(SseCodec, ParseAcrossSplits) {
    const std::string event = "id:42\ndata:hello\n\n";
    SseParser p;
    std::vector<SseEvent> got;
    for (char c : event) {
        auto e = p.feed(std::string_view(&c, 1));
        got.insert(got.end(), e.begin(), e.end());
    }
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].id.value(), "42");
    EXPECT_EQ(got[0].data, "hello");
    EXPECT_EQ(p.last_event_id(), "42");
}

TEST(SseCodec, CommentIgnored) {
    const std::string stream = ":keepalive\ndata:x\n\n";
    SseParser p;
    auto events = p.feed(stream);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "x");
}

TEST(SocketUtil, ListenEphemeralPort) {
    std::uint16_t port = 0;
    int fd = mcp::detail::http::listen_tcp("127.0.0.1", 0, port);
    ASSERT_GE(fd, 0);
    EXPECT_GT(port, 0);
    ::close(fd);
}

TEST(SocketUtil, ConnectRoundTrip) {
    std::uint16_t port = 0;
    int listen = mcp::detail::http::listen_tcp("127.0.0.1", 0, port);
    ASSERT_GE(listen, 0);

    int client = mcp::detail::http::connect_tcp("127.0.0.1", port);
    ASSERT_GE(client, 0);
    int accepted = mcp::detail::http::accept_connection(listen);
    ASSERT_GE(accepted, 0);

    const char* msg = "ping";
    ASSERT_EQ(::write(client, msg, 4), 4);
    char buf[4] = {};
    ASSERT_EQ(::read(accepted, buf, 4), 4);
    EXPECT_EQ(std::string(buf, 4), "ping");

    ::close(accepted);
    ::close(client);
    ::close(listen);
}

}  // namespace