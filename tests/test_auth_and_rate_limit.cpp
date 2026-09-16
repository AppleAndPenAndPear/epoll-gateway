#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include "config.h"
#include "api_key_manager.h"
#include "rate_limiter_manager.h"
#include "route_utils.h"

TEST(ApiKeyManagerTest, ValidatesKnownKey) {
    std::vector<ApiKeyConfig> keys = {
        {"test-key-123", "demo", {20, 5}},
        {"premium-key-456", "premium", {50, 10}}
    };

    ApiKeyManager manager(keys);
    EXPECT_TRUE(manager.validate("test-key-123"));
    EXPECT_TRUE(manager.validate("premium-key-456"));
    EXPECT_FALSE(manager.validate("unknown-key"));
}

TEST(ApiKeyManagerTest, ReturnsConfigForExistingKey) {
    std::vector<ApiKeyConfig> keys = {{"test-key-123", "demo", {20, 5}}};
    ApiKeyManager manager(keys);

    const auto* cfg = manager.get("test-key-123");
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->name, "demo");
    EXPECT_EQ(cfg->rate_limit.capacity, 20U);
    EXPECT_EQ(cfg->rate_limit.refill_per_second, 5U);
}

TEST(ApiKeyManagerTest, BindsKeyToRouteHostAndTenantPolicy) {
    ApiKeyConfig key;
    key.key = "tenant-a-key";
    key.allowed_hosts = {"api.example.com"};
    key.allowed_tenants = {"tenant-a"};

    ApiKeyManager manager({key});
    GatewayRoute route;
    route.method = "GET";
    route.path = "/v1/orders/*";
    route.host = "api.example.com";
    route.tenant = "tenant-a";
    route.allowed_api_keys = {"tenant-a-key"};

    EXPECT_TRUE(manager.authorize("tenant-a-key", route, "api.example.com:443", "tenant-a"));
    EXPECT_FALSE(manager.authorize("tenant-a-key", route, "api.other.com", "tenant-a"));
    EXPECT_FALSE(manager.authorize("tenant-a-key", route, "api.example.com", "tenant-b"));
}

TEST(ConfigTest, LoadsRouteTargetAndApiKeyScopes) {
    const std::string path = "/tmp/epollthread-config-test.json";
    std::ofstream output(path);
    output << R"({
        "routes": [{
            "name": "orders",
            "method": "GET",
            "path": "/v1/orders/*",
            "host": "api.example.com",
            "tenant": "tenant-a",
            "upstream_target": {"name": "orders-service", "timeout_ms": 1200},
            "allowed_api_keys": ["tenant-a-key"]
        }],
        "api_keys": [{
            "key": "tenant-a-key",
            "allowed_hosts": ["api.example.com"],
            "allowed_tenants": ["tenant-a"]
        }]
    })";
    output.close();

    Config config = Config::from_file(path);
    std::remove(path.c_str());

    ASSERT_EQ(config.upstream_config.routes.size(), 1U);
    EXPECT_EQ(config.upstream_config.routes[0].upstream_target.name, "orders-service");
    EXPECT_EQ(config.upstream_config.routes[0].upstream_target.timeout_ms, 1200);
    ASSERT_EQ(config.api_keys.size(), 1U);
    EXPECT_EQ(config.api_keys[0].allowed_tenants[0], "tenant-a");
}

TEST(RateLimiterManagerTest, AllowsTokenUntilCapacityExhausted) {
    RateLimitConfig cfg{2, 2};
    RateLimiterManager manager(cfg);

    EXPECT_TRUE(manager.try_acquire("client-a"));
    EXPECT_TRUE(manager.try_acquire("client-a"));
    EXPECT_FALSE(manager.try_acquire("client-a"));
}

TEST(RateLimiterManagerTest, DifferentKeysUseSeparateBuckets) {
    RateLimitConfig cfg{1, 1};
    RateLimiterManager manager(cfg);

    EXPECT_TRUE(manager.try_acquire("client-a"));
    EXPECT_TRUE(manager.try_acquire("client-b"));
    EXPECT_FALSE(manager.try_acquire("client-a"));
}
