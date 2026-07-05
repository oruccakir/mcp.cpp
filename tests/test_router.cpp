#include <gtest/gtest.h>

#include <mcp/core/router.hpp>

namespace {

using namespace mcp;

Message make_request(std::int64_t id, const std::string& method,
                     std::optional<json> params = std::nullopt) {
    return Message(JsonRpcRequest{RequestId(id), method, std::move(params)});
}

TEST(Router, DispatchesExactRequest) {
    MessageRouter router;
    router.set_request_handler(
        "tools/list", [](const std::optional<json>&) -> Result<json> {
            return json{{"tools", json::array()}};
        });

    auto response = router.dispatch(make_request(1, "tools/list"));
    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->is_error());
    EXPECT_TRUE(response->result->contains("tools"));
    EXPECT_EQ(std::get<std::int64_t>(*response->id), 1);
}

TEST(Router, HandlerErrorBecomesErrorResponse) {
    MessageRouter router;
    router.set_request_handler(
        "fail", [](const std::optional<json>&) -> Result<json> {
            return Error(ErrorCode::ToolExecutionFailed, "boom");
        });

    auto response = router.dispatch(make_request(2, "fail"));
    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(response->is_error());
    EXPECT_EQ(response->error->code, -32004);
}

TEST(Router, UnknownMethodIsMethodNotFound) {
    // FR-CORE-011
    MessageRouter router;
    auto response = router.dispatch(make_request(3, "no/such"));
    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(response->is_error());
    EXPECT_EQ(response->error->code, static_cast<int>(ErrorCode::MethodNotFound));
}

TEST(Router, ThrowingHandlerBecomesInternalError) {
    MessageRouter router;
    router.set_request_handler("throws",
                               [](const std::optional<json>&) -> Result<json> {
                                   throw std::runtime_error("unexpected");
                               });
    auto response = router.dispatch(make_request(4, "throws"));
    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(response->is_error());
    EXPECT_EQ(response->error->code, static_cast<int>(ErrorCode::InternalError));
}

TEST(Router, WildcardMatchesPrefix) {
    // FR-CORE-013
    MessageRouter router;
    router.set_request_handler(
        "experimental/*", [](const std::optional<json>&) -> Result<json> {
            return json{{"via", "wildcard"}};
        });
    router.set_request_handler(
        "experimental/exact", [](const std::optional<json>&) -> Result<json> {
            return json{{"via", "exact"}};
        });

    auto wild = router.dispatch(make_request(5, "experimental/anything"));
    EXPECT_EQ(wild->result->at("via"), "wildcard");

    auto exact = router.dispatch(make_request(6, "experimental/exact"));
    EXPECT_EQ(exact->result->at("via"), "exact");

    auto miss = router.dispatch(make_request(7, "other/thing"));
    EXPECT_TRUE(miss->is_error());
}

TEST(Router, NotificationDispatch) {
    MessageRouter router;
    int calls = 0;
    router.set_notification_handler(
        "notifications/test",
        [&calls](const std::optional<json>&) { ++calls; });

    auto r1 = router.dispatch(Message(JsonRpcNotification{"notifications/test", {}}));
    EXPECT_FALSE(r1.has_value());  // Notifications never produce responses.
    EXPECT_EQ(calls, 1);

    // Unknown notifications are dropped silently (FR-CORE-012).
    auto r2 = router.dispatch(Message(JsonRpcNotification{"notifications/unknown", {}}));
    EXPECT_FALSE(r2.has_value());
}

TEST(Router, ResponseMatchingById) {
    // FR-CORE-012: responses match pending requests by id.
    MessageRouter router;
    std::optional<Result<json>> received;
    router.register_pending(RequestId(std::int64_t{9}),
                            [&received](Result<json> r) { received = std::move(r); });

    JsonRpcResponse response;
    response.id = RequestId(std::int64_t{9});
    response.result = json{{"done", true}};
    router.dispatch(Message(response));

    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(received->has_value());
    EXPECT_TRUE(received->value().at("done").get<bool>());

    // Second delivery of the same id: callback already consumed.
    received.reset();
    router.dispatch(Message(response));
    EXPECT_FALSE(received.has_value());
}

TEST(Router, FailPending) {
    MessageRouter router;
    std::optional<Result<json>> received;
    router.register_pending(RequestId(std::string("a")),
                            [&received](Result<json> r) { received = std::move(r); });

    EXPECT_TRUE(router.fail_pending(RequestId(std::string("a")),
                                    Error(ErrorCode::InternalError, "timeout")));
    ASSERT_TRUE(received.has_value());
    EXPECT_FALSE(received->has_value());
    EXPECT_EQ(received->error().message, "timeout");

    EXPECT_FALSE(router.fail_pending(RequestId(std::string("a")),
                                     Error(ErrorCode::InternalError, "again")));
}

TEST(Router, FailAllPending) {
    MessageRouter router;
    int failures = 0;
    for (std::int64_t i = 0; i < 3; ++i) {
        router.register_pending(RequestId(i), [&failures](Result<json> r) {
            if (!r) {
                ++failures;
            }
        });
    }
    router.fail_all_pending(Error(ErrorCode::InternalError, "closed"));
    EXPECT_EQ(failures, 3);
}

}  // namespace
