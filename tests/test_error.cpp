#include <gtest/gtest.h>

#include <mcp/error.hpp>
#include <mcp/result.hpp>

namespace {

using mcp::Error;
using mcp::ErrorCode;

TEST(ErrorCodes, JsonRpcStandardValues) {
    // FR-CORE-002
    EXPECT_EQ(static_cast<int>(ErrorCode::ParseError), -32700);
    EXPECT_EQ(static_cast<int>(ErrorCode::InvalidRequest), -32600);
    EXPECT_EQ(static_cast<int>(ErrorCode::MethodNotFound), -32601);
    EXPECT_EQ(static_cast<int>(ErrorCode::InvalidParams), -32602);
    EXPECT_EQ(static_cast<int>(ErrorCode::InternalError), -32603);
}

TEST(ErrorCodes, McpSpecificValues) {
    // FR-CORE-003
    EXPECT_EQ(static_cast<int>(ErrorCode::ConnectionNotInitialized), -32000);
    EXPECT_EQ(static_cast<int>(ErrorCode::RequestAlreadyInProgress), -32001);
    EXPECT_EQ(static_cast<int>(ErrorCode::CapabilityNotSupported), -32002);
    EXPECT_EQ(static_cast<int>(ErrorCode::ResourceNotFound), -32003);
    EXPECT_EQ(static_cast<int>(ErrorCode::ToolExecutionFailed), -32004);
    EXPECT_EQ(static_cast<int>(ErrorCode::InvalidUri), -32005);
    EXPECT_EQ(static_cast<int>(ErrorCode::PaginationError), -32006);
}

TEST(Error, JsonRoundTrip) {
    Error e(ErrorCode::InvalidParams, "bad params", mcp::json{{"detail", 42}});
    const auto j = e.to_json();
    EXPECT_EQ(j.at("code").get<int>(), -32602);
    EXPECT_EQ(j.at("message").get<std::string>(), "bad params");

    const Error parsed = Error::from_json(j);
    EXPECT_EQ(parsed.code, e.code);
    EXPECT_EQ(parsed.message, e.message);
    ASSERT_TRUE(parsed.data.has_value());
    EXPECT_EQ(parsed.data->at("detail").get<int>(), 42);
}

TEST(Error, OmitsAbsentData) {
    Error e(ErrorCode::InternalError, "boom");
    EXPECT_FALSE(e.to_json().contains("data"));
}

TEST(Exceptions, HierarchyCarriesError) {
    // FR-ERR-002: McpError is the common base.
    try {
        throw mcp::CapabilityError("tools not negotiated");
    } catch (const mcp::McpError& e) {
        EXPECT_EQ(e.code(), -32002);
        EXPECT_STREQ(e.what(), "tools not negotiated");
    }

    try {
        throw mcp::JsonRpcError(Error(ErrorCode::MethodNotFound, "nope"));
    } catch (const mcp::McpError& e) {
        EXPECT_EQ(e.code(), -32601);
    }

    try {
        throw mcp::TransportError("pipe broke");
    } catch (const mcp::McpError& e) {
        EXPECT_EQ(e.error().message, "pipe broke");
    }
}

TEST(ResultType, ValueAndError) {
    mcp::Result<int> ok = 5;
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok.value(), 5);
    EXPECT_EQ(ok.value_or(9), 5);

    mcp::Result<int> err = Error(ErrorCode::InternalError, "x");
    ASSERT_FALSE(err);
    EXPECT_EQ(err.error().code, -32603);
    EXPECT_EQ(err.value_or(9), 9);

    mcp::Result<void> vok;
    EXPECT_TRUE(vok);
    mcp::Result<void> verr = Error(ErrorCode::InternalError, "y");
    EXPECT_FALSE(verr);
    EXPECT_EQ(verr.error().message, "y");
}

}  // namespace
