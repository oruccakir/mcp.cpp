#include <gtest/gtest.h>

#include <mcp/detail/schema.hpp>

namespace {

using mcp::json;
using mcp::detail::validate_schema;

TEST(Schema, TypeChecks) {
    EXPECT_TRUE(validate_schema(json::object(), json{{"type", "object"}}));
    EXPECT_FALSE(validate_schema(json("x"), json{{"type", "object"}}));
    EXPECT_TRUE(validate_schema(json(3), json{{"type", "integer"}}));
    EXPECT_FALSE(validate_schema(json(3.5), json{{"type", "integer"}}));
    EXPECT_TRUE(validate_schema(json(3.5), json{{"type", "number"}}));
    EXPECT_TRUE(validate_schema(json(nullptr), json{{"type", "null"}}));
    // Type unions.
    const json schema{{"type", json::array({"string", "integer"})}};
    EXPECT_TRUE(validate_schema(json("a"), schema));
    EXPECT_TRUE(validate_schema(json(1), schema));
    EXPECT_FALSE(validate_schema(json(true), schema));
}

TEST(Schema, RequiredAndProperties) {
    const json schema{
        {"type", "object"},
        {"properties",
         {{"message", {{"type", "string"}}}, {"count", {{"type", "integer"}}}}},
        {"required", json::array({"message"})}};

    EXPECT_TRUE(validate_schema(json{{"message", "hi"}}, schema));
    EXPECT_TRUE(validate_schema(json{{"message", "hi"}, {"count", 2}}, schema));

    auto missing = validate_schema(json::object(), schema);
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().message.find("message"), std::string::npos);

    EXPECT_FALSE(validate_schema(json{{"message", 42}}, schema));
    EXPECT_FALSE(validate_schema(json{{"message", "hi"}, {"count", "two"}}, schema));
}

TEST(Schema, AdditionalPropertiesFalse) {
    const json schema{{"type", "object"},
                      {"properties", {{"a", {{"type", "string"}}}}},
                      {"additionalProperties", false}};
    EXPECT_TRUE(validate_schema(json{{"a", "x"}}, schema));
    EXPECT_FALSE(validate_schema(json{{"a", "x"}, {"b", 1}}, schema));
}

TEST(Schema, ItemsAndEnum) {
    const json arr_schema{{"type", "array"}, {"items", {{"type", "integer"}}}};
    EXPECT_TRUE(validate_schema(json::array({1, 2, 3}), arr_schema));
    EXPECT_FALSE(validate_schema(json::array({1, "two"}), arr_schema));

    const json enum_schema{{"enum", json::array({"red", "green"})}};
    EXPECT_TRUE(validate_schema(json("red"), enum_schema));
    EXPECT_FALSE(validate_schema(json("blue"), enum_schema));
}

TEST(Schema, EmptySchemaAcceptsEverything) {
    EXPECT_TRUE(validate_schema(json{{"anything", 1}}, json::object()));
    EXPECT_TRUE(validate_schema(json(42), json(true)));
}

}  // namespace
