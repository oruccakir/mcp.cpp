#include <gtest/gtest.h>

#include <mcp/jsonrpc/message.hpp>

namespace {

using namespace mcp;

TEST(MessageParse, ValidRequest) {
    const auto j = json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"cursor":"abc"}})");
    auto result = parse_message(j);
    ASSERT_TRUE(result);
    const auto& request = std::get<JsonRpcRequest>(result.value());
    EXPECT_EQ(request.method, "tools/list");
    EXPECT_EQ(std::get<std::int64_t>(request.id), 1);
    ASSERT_TRUE(request.params.has_value());
    EXPECT_EQ(request.params->at("cursor").get<std::string>(), "abc");
}

TEST(MessageParse, StringIdRequest) {
    const auto j = json::parse(R"({"jsonrpc":"2.0","id":"req-7","method":"ping"})");
    auto result = parse_message(j);
    ASSERT_TRUE(result);
    const auto& request = std::get<JsonRpcRequest>(result.value());
    EXPECT_EQ(std::get<std::string>(request.id), "req-7");
    EXPECT_FALSE(request.params.has_value());
}

TEST(MessageParse, Notification) {
    const auto j = json::parse(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    auto result = parse_message(j);
    ASSERT_TRUE(result);
    const auto& note = std::get<JsonRpcNotification>(result.value());
    EXPECT_EQ(note.method, "notifications/initialized");
}

TEST(MessageParse, SuccessResponse) {
    const auto j = json::parse(R"({"jsonrpc":"2.0","id":3,"result":{"ok":true}})");
    auto result = parse_message(j);
    ASSERT_TRUE(result);
    const auto& response = std::get<JsonRpcResponse>(result.value());
    ASSERT_TRUE(response.id.has_value());
    EXPECT_EQ(std::get<std::int64_t>(*response.id), 3);
    EXPECT_FALSE(response.is_error());
    EXPECT_TRUE(response.result->at("ok").get<bool>());
}

TEST(MessageParse, ErrorResponseWithNullId) {
    const auto j = json::parse(
        R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse"}})");
    auto result = parse_message(j);
    ASSERT_TRUE(result);
    const auto& response = std::get<JsonRpcResponse>(result.value());
    EXPECT_FALSE(response.id.has_value());
    ASSERT_TRUE(response.is_error());
    EXPECT_EQ(response.error->code, -32700);
}

TEST(MessageParse, RejectsInvalidShapes) {
    const char* cases[] = {
        R"([1,2])",                                             // not an object (single)
        R"({"id":1,"method":"x"})",                             // missing jsonrpc
        R"({"jsonrpc":"1.0","id":1,"method":"x"})",             // wrong version
        R"({"jsonrpc":2.0,"id":1,"method":"x"})",               // non-string version
        R"({"jsonrpc":"2.0","id":1,"method":5})",               // non-string method
        R"({"jsonrpc":"2.0","id":{},"method":"x"})",            // bad id type
        R"({"jsonrpc":"2.0","id":1,"method":"x","params":3})",  // scalar params
        R"({"jsonrpc":"2.0","id":1})",                          // no method/result/error
        R"({"jsonrpc":"2.0","id":1,"result":1,"error":{"code":1,"message":"m"}})",
        R"({"jsonrpc":"2.0","result":1})",                      // response missing id
        R"({"jsonrpc":"2.0","id":1,"error":{"code":1}})",       // error missing message
    };
    for (const char* text : cases) {
        auto result = parse_message(json::parse(text));
        ASSERT_FALSE(result) << text;
        EXPECT_EQ(result.error().code,
                  static_cast<int>(ErrorCode::InvalidRequest))
            << text;
    }
}

TEST(MessageSerialize, RequestIsSingleLine) {
    JsonRpcRequest request{RequestId(std::int64_t{42}), "tools/call",
                           json{{"name", "echo"}}};
    const auto line = serialize_message(Message(request));
    EXPECT_EQ(line.find('\n'), std::string::npos);  // FR-TRAN-003

    const auto j = json::parse(line);
    EXPECT_EQ(j.at("jsonrpc"), "2.0");
    EXPECT_EQ(j.at("id"), 42);
    EXPECT_EQ(j.at("method"), "tools/call");
}

TEST(MessageSerialize, EmbeddedNewlinesAreEscaped) {
    JsonRpcRequest request{RequestId(std::int64_t{1}), "echo",
                           json{{"text", "line1\nline2"}}};
    const auto line = serialize_message(Message(request));
    EXPECT_EQ(line.find('\n'), std::string::npos);

    auto parsed = parse_frame(line);
    ASSERT_TRUE(parsed);
    const auto& round =
        std::get<JsonRpcRequest>(parsed.value().messages.at(0));
    EXPECT_EQ(round.params->at("text").get<std::string>(), "line1\nline2");
}

TEST(MessageSerialize, NullIdErrorResponse) {
    JsonRpcResponse response;
    response.error = Error(ErrorCode::ParseError, "bad json");
    const auto j = json::parse(serialize_message(Message(response)));
    EXPECT_TRUE(j.at("id").is_null());
    EXPECT_EQ(j.at("error").at("code"), -32700);
}

TEST(ParseFrame, InvalidJsonIsParseError) {
    auto result = parse_frame("{not json");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, static_cast<int>(ErrorCode::ParseError));
}

TEST(ParseFrame, EmptyBatchIsInvalidRequest) {
    auto result = parse_frame("[]");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, static_cast<int>(ErrorCode::InvalidRequest));
}

TEST(ParseFrame, BatchMixedValidity) {
    // FR-CORE-001: batch support; invalid items become per-item errors.
    const auto text =
        R"([{"jsonrpc":"2.0","id":1,"method":"ping"},)"
        R"({"bogus":true},)"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"}])";
    auto result = parse_frame(text);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().was_batch);
    EXPECT_EQ(result.value().messages.size(), 2u);
    EXPECT_EQ(result.value().item_errors.size(), 1u);
}

TEST(ParseFrame, BatchSerializeRoundTrip) {
    std::vector<Message> batch;
    batch.emplace_back(JsonRpcRequest{RequestId(std::int64_t{1}), "ping", {}});
    batch.emplace_back(JsonRpcNotification{"notifications/initialized", {}});

    const auto line = serialize_batch(batch);
    EXPECT_EQ(line.find('\n'), std::string::npos);

    auto parsed = parse_frame(line);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed.value().was_batch);
    ASSERT_EQ(parsed.value().messages.size(), 2u);
    EXPECT_TRUE(parsed.value().item_errors.empty());
}

}  // namespace
