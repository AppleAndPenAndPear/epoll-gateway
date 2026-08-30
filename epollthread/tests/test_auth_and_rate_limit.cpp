#include <gtest/gtest.h>
#include "config.h"
#include "api_key_manager.h"
#include "rate_limiter_manager.h"

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
