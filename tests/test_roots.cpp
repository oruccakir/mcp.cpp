#include <gtest/gtest.h>

#include <mcp/client/roots_provider.hpp>

namespace {

using namespace mcp;

TEST(RootsProvider, FileSchemeRequired) {
    // FR-CLI-004
    RootsProvider provider;
    EXPECT_TRUE(provider.add_root(Root{"file:///home/user/project", "Project"}));

    auto bad = provider.add_root(Root{"https://example.com", std::nullopt});
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, static_cast<int>(ErrorCode::InvalidUri));

    EXPECT_FALSE(provider.add_root(Root{"file:///home/user/project", "dup"}));
    EXPECT_EQ(provider.size(), 1u);
}

TEST(RootsProvider, ListAndSerialize) {
    RootsProvider provider;
    ASSERT_TRUE(provider.add_root(Root{"file:///a", "A"}));
    ASSERT_TRUE(provider.add_root(Root{"file:///b", std::nullopt}));

    const auto roots = provider.list_roots();
    ASSERT_EQ(roots.size(), 2u);

    const json j = roots;
    EXPECT_EQ(j[0].at("uri"), "file:///a");
    EXPECT_EQ(j[0].at("name"), "A");
    EXPECT_FALSE(j[1].contains("name"));  // omit-if-absent
}

TEST(RootsProvider, ChangedCallback) {
    // FR-CLI-005
    RootsProvider provider;
    int changes = 0;
    provider.set_changed_callback([&changes] { ++changes; });

    ASSERT_TRUE(provider.add_root(Root{"file:///a", std::nullopt}));
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(provider.remove_root("file:///a"));
    EXPECT_EQ(changes, 2);
    EXPECT_FALSE(provider.remove_root("file:///a"));
    EXPECT_EQ(changes, 2);
}

}  // namespace
