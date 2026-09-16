#include <gtest/gtest.h>
#include "config.h"
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

TEST(RouteUtilsTest, RouteMetadataMatchByHostMethodAndTenant) {
    GatewayRoute route;
    route.method = "GET";
    route.path = "/v1/users/{id}";
    route.host = "api.example.com";
    route.tenant = "tenant-a";

    std::map<std::string, std::string> params;
    EXPECT_TRUE(routeMatchesRequest(route, "GET", "api.example.com", "tenant-a", "/v1/users/123", params));
    EXPECT_EQ(params["id"], "123");
}

TEST(RouteUtilsTest, RouteMetadataRejectsWrongHostOrTenant) {
    GatewayRoute route;
    route.method = "GET";
    route.path = "/v1/users/{id}";
    route.host = "api.example.com";
    route.tenant = "tenant-a";

    std::map<std::string, std::string> params;
    EXPECT_FALSE(routeMatchesRequest(route, "GET", "api.other.com", "tenant-a", "/v1/users/123", params));
    EXPECT_FALSE(routeMatchesRequest(route, "GET", "api.example.com", "tenant-b", "/v1/users/123", params));
    EXPECT_FALSE(routeMatchesRequest(route, "POST", "api.example.com", "tenant-a", "/v1/users/123", params));
}

TEST(RouteUtilsTest, SeparatesPathMatchFromMethodMatch) {
    GatewayRoute route;
    route.method = "GET";
    route.path = "/v1/orders";

    std::map<std::string, std::string> params;
    EXPECT_TRUE(routeMatchesPath(route, "api.example.com", "tenant-a", "/v1/orders", params));
    EXPECT_FALSE(routeMatchesRequest(route, "POST", "api.example.com", "tenant-a", "/v1/orders", params));
}

TEST(RouteUtilsTest, StaticRootRejectsPathTraversal) {
    std::string resolved;
    EXPECT_FALSE(is_safe_static_path("/static", "/static/../../etc/passwd", "/var/www", resolved));
    EXPECT_FALSE(is_safe_static_path("/static", "/static/..", "/var/www", resolved));
    EXPECT_TRUE(is_safe_static_path("/static", "/static/index.html", "/var/www", resolved));
    EXPECT_TRUE(resolved.find("/var/www/index.html") != std::string::npos);
}