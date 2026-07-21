#include <gtest/gtest.h>
#include "route_utils.h"

TEST(RouteUtilsTest, ExactMatch) {
    std::map<std::string, std::string> params;
    EXPECT_TRUE(matchRoute("/users", "/users", params));
    EXPECT_TRUE(params.empty());
}

TEST(RouteUtilsTest, SingleParam) {
    std::map<std::string, std::string> params;
    EXPECT_TRUE(matchRoute("/users/{id}", "/users/123", params));
    ASSERT_EQ(params.size(), 1);
    EXPECT_EQ(params["id"], "123");
}

TEST(RouteUtilsTest, MultipleParams) {
    std::map<std::string, std::string> params;
    EXPECT_TRUE(matchRoute("/users/{userId}/posts/{postId}", "/users/5/posts/42", params));
    ASSERT_EQ(params.size(), 2);
    EXPECT_EQ(params["userId"], "5");
    EXPECT_EQ(params["postId"], "42");
}

TEST(RouteUtilsTest, MismatchedSegmentCount) {
    std::map<std::string, std::string> params;
    EXPECT_FALSE(matchRoute("/users/{id}", "/users/123/extra", params));
    EXPECT_FALSE(matchRoute("/users/{id}/posts", "/users/123", params));
}

TEST(RouteUtilsTest, NonParamMismatch) {
    std::map<std::string, std::string> params;
    EXPECT_FALSE(matchRoute("/users/{id}", "/admin/123", params));
}