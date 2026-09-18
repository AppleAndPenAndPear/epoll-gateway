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

TEST(UpstreamManagerTest, HealthSnapshotReportsHealthyAndTotalPerUpstream) {
    UpstreamConfig config;
    // Port 1 is not listening, so check_health() must mark it unhealthy.
    config.upstreams["down"] = Upstream{
        {UpstreamServer{"127.0.0.1", 1}},
        "round_robin"
    };
    config.upstreams["mixed"] = Upstream{
        {UpstreamServer{"127.0.0.1", 1}, UpstreamServer{"127.0.0.1", 1}},
        "round_robin"
    };

    UpstreamManager manager(config);
    auto snapshot = manager.health_snapshot();
    ASSERT_EQ(snapshot.size(), 2u);
    EXPECT_EQ(snapshot["down"], std::make_pair(static_cast<size_t>(1), static_cast<size_t>(1)))
        << "snapshot should reflect the initial healthy=true default before any probe";
    EXPECT_EQ(snapshot["mixed"], std::make_pair(static_cast<size_t>(2), static_cast<size_t>(2)));

    manager.check_health();
    snapshot = manager.health_snapshot();
    EXPECT_EQ(snapshot["down"], std::make_pair(static_cast<size_t>(0), static_cast<size_t>(1)));
    EXPECT_EQ(snapshot["mixed"], std::make_pair(static_cast<size_t>(0), static_cast<size_t>(2)));
}

TEST(UpstreamManagerTest, StatusSnapshotReportsHealthAndCircuitState) {
    UpstreamConfig config;
    config.upstreams["svc"] = Upstream{
        {UpstreamServer{"127.0.0.1", 1}},
        "round_robin"
    };

    UpstreamManager manager(config);
    const auto& server = manager.config().upstreams.at("svc").servers.at(0);

    auto status = manager.status_snapshot();
    ASSERT_EQ(status.size(), 1u);
    EXPECT_EQ(status[0].name, "svc");
    ASSERT_EQ(status[0].backends.size(), 1u);
    EXPECT_TRUE(status[0].backends[0].healthy);
    EXPECT_FALSE(status[0].backends[0].circuit_open);

    // Drive the circuit open (threshold 2), then check the snapshot reflects it
    manager.record_failure("svc", server, 2, 10000);
    manager.record_failure("svc", server, 2, 10000);
    status = manager.status_snapshot();
    EXPECT_TRUE(status[0].backends[0].circuit_open);
}
