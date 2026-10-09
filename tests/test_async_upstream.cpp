#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "upstream_async.h"
#include "config.h"
#include "upstream_manager.h"

TEST(UpstreamGroupIdTest, PrefersConfiguredUpstreamTargetName) {
    GatewayRoute route;
    route.name = "legacy-route";
    route.upstream_target.name = "orders-service";

    EXPECT_EQ(resolve_upstream_group_id(route), "orders-service");
}

TEST(UpstreamGroupIdTest, FallsBackToRouteNameWhenTargetIsEmpty) {
    GatewayRoute route;
    route.name = "legacy-route";

    EXPECT_EQ(resolve_upstream_group_id(route), "legacy-route");
}

TEST(BoundedAsyncUpstreamExecutorTest, ExecutesAcceptedTasks) {
    BoundedAsyncUpstreamExecutor executor(2, 2);
    std::atomic<int> count{0};

    EXPECT_EQ(executor.submit([&] { count.fetch_add(1, std::memory_order_relaxed); }),
              SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit([&] { count.fetch_add(1, std::memory_order_relaxed); }),
              SubmitStatus::Accepted);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(count.load(std::memory_order_relaxed), 2);

    executor.shutdown();
}

TEST(BoundedAsyncUpstreamExecutorTest, RejectsWhenQueueIsFull) {
    BoundedAsyncUpstreamExecutor executor(1, 0);

    EXPECT_EQ(executor.submit([] {}), SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit([] {}), SubmitStatus::QueueFull);

    executor.shutdown();
}

TEST(BoundedAsyncUpstreamExecutorTest, ShutdownPreventsNewSubmissions) {
    BoundedAsyncUpstreamExecutor executor(1, 1);
    executor.shutdown();

    EXPECT_EQ(executor.submit([] {}), SubmitStatus::Shutdown);
}

TEST(BoundedAsyncUpstreamExecutorTest, RejectsSameUpstreamGroupWhenPerGroupLimitIsReached) {
    BoundedAsyncUpstreamExecutor executor(2, 2);

    EXPECT_EQ(executor.submit("svc-a", [] {}), SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit("svc-a", [] {}), SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit("svc-a", [] {}), SubmitStatus::QueueFull);

    executor.shutdown();
}

TEST(BoundedAsyncUpstreamExecutorTest, SameGroupIsIsolatedFromOtherUpstreamGroups) {
    BoundedAsyncUpstreamExecutor executor(2, 2);

    EXPECT_EQ(executor.submit("svc-a", [] {}), SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit("svc-a", [] {}), SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit("svc-b", [] {}), SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit("svc-a", [] {}), SubmitStatus::QueueFull);

    executor.shutdown();
}

TEST(UpstreamFlowTest, RouteGroupResolvesToHealthyServerWithinGroupAndIsolatesExecutorBuckets) {
    GatewayRoute route;
    route.name = "orders-route";
    route.upstream_target.name = "orders-service";

    EXPECT_EQ(resolve_upstream_group_id(route), "orders-service");

    UpstreamConfig config;
    config.upstreams["orders-service"] = Upstream{
        {
            UpstreamServer{"10.0.0.1", 8080, false, "", "", false, true},
            UpstreamServer{"10.0.0.2", 8080, false, "", "", false, true}
        },
        "round_robin"
    };

    UpstreamManager manager(config);
    EXPECT_EQ(manager.pick_server("orders-service").host, "10.0.0.1");
    EXPECT_EQ(manager.pick_server("orders-service").host, "10.0.0.2");

    BoundedAsyncUpstreamExecutor executor(2, 2);
    EXPECT_EQ(executor.submit("orders-service", [] {}), SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit("orders-service", [] {}), SubmitStatus::Accepted);
    EXPECT_EQ(executor.submit("orders-service", [] {}), SubmitStatus::QueueFull);
    EXPECT_EQ(executor.submit("payments-service", [] {}), SubmitStatus::Accepted);

    executor.shutdown();
}
