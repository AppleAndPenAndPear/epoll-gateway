#include <gtest/gtest.h>

#include <chrono>

#include "upstream_manager.h"

TEST(UpstreamManagerTest, HealthCheckMarksUnavailableServerUnhealthy) {
    UpstreamConfig config;
    config.health_check_timeout_ms = 50;
    config.upstreams["unavailable"] = Upstream{
        {UpstreamServer{"127.0.0.1", 1}},
        "round_robin"
    };

    UpstreamManager manager(config);
    auto start = std::chrono::steady_clock::now();
    manager.check_health();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    const auto& server = manager.config().upstreams.at("unavailable").servers.at(0);
    EXPECT_FALSE(server.healthy);
    EXPECT_GE(server.consecutive_failures, 1);
    EXPECT_LT(elapsed.count(), 500);
}

TEST(UpstreamManagerTest, CircuitOpensAfterFailuresAndAllowsRecoveryProbe) {
    UpstreamConfig config;
    config.upstreams["backend"] = Upstream{
        {UpstreamServer{"127.0.0.1", 1}},
        "round_robin"
    };

    UpstreamManager manager(config);
    const auto& server = manager.config().upstreams.at("backend").servers.at(0);

    EXPECT_TRUE(manager.allow_request("backend", server, 0));
    manager.record_failure("backend", server, 2, 0);
    EXPECT_TRUE(manager.allow_request("backend", server, 0));
    manager.record_failure("backend", server, 2, 10000);
    EXPECT_FALSE(manager.allow_request("backend", server, 10000));

    manager.record_success("backend", server);
    EXPECT_TRUE(manager.allow_request("backend", server, 10000));
}
