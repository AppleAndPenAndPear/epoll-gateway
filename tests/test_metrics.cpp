#include <gtest/gtest.h>
#include "metrics.h"
#include <string>

// The route/host/tenant dimension is the only place operators can attribute a
// latency spike or a failure burst to a specific route, so these tests lock the
// two properties the new metrics must guarantee: the histogram buckets follow
// the global boundaries (so P95/P99 are comparable), and the failure counters
// are broken down by cause rather than collapsed into one number.
//
// Metrics is a process-wide singleton, so every test uses a unique route name;
// the shared map then keeps the assertions independent of test ordering.

namespace {

// Return the numeric value of the first exposition-format line that starts with
// `prefix` (which already includes the trailing space before the value).
bool find_metric_value(const std::string& body, const std::string& prefix, double& out) {
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        const std::string line = body.substr(pos, eol - pos);
        if (line.rfind(prefix, 0) == 0) {
            const size_t sp = line.rfind(' ');
            if (sp != std::string::npos) {
                out = std::stod(line.substr(sp + 1));
                return true;
            }
        }
        if (eol == body.size()) break;
        pos = eol + 1;
    }
    return false;
}

}  // namespace

TEST(MetricsRouteHistogram, ExportsCountersAndBuckets) {
    auto& m = Metrics::instance();
    const std::string route = "__test_hist_a__";
    const std::string base = "route=\"" + route + "\",host=\"host-a\",tenant=\"tenant-a\"";

    // One fast success (3 ms) and one slow server error (300 ms).
    m.record_route_request(route, "host-a", "tenant-a", 200, 0.003);
    m.record_route_request(route, "host-a", "tenant-a", 500, 0.3);
    const std::string out = m.to_string();

    double v = 0.0;
    ASSERT_TRUE(find_metric_value(out, "epoll_route_requests_total{" + base + ",code=\"2xx\"} ", v));
    EXPECT_DOUBLE_EQ(v, 1.0);
    ASSERT_TRUE(find_metric_value(out, "epoll_route_requests_total{" + base + ",code=\"5xx\"} ", v));
    EXPECT_DOUBLE_EQ(v, 1.0);

    ASSERT_TRUE(find_metric_value(out, "epoll_route_request_duration_seconds_count{" + base + "} ", v));
    EXPECT_DOUBLE_EQ(v, 2.0);
    ASSERT_TRUE(find_metric_value(out, "epoll_route_request_duration_seconds_sum{" + base + "} ", v));
    EXPECT_NEAR(v, 0.303, 1e-9);

    // 3 ms lands in the first finite bucket; 300 ms only reaches le="0.5".
    ASSERT_TRUE(find_metric_value(out, "epoll_route_request_duration_seconds_bucket{" + base + ",le=\"0.005\"} ", v));
    EXPECT_DOUBLE_EQ(v, 1.0);
    ASSERT_TRUE(find_metric_value(out, "epoll_route_request_duration_seconds_bucket{" + base + ",le=\"0.25\"} ", v));
    EXPECT_DOUBLE_EQ(v, 1.0);
    ASSERT_TRUE(find_metric_value(out, "epoll_route_request_duration_seconds_bucket{" + base + ",le=\"0.5\"} ", v));
    EXPECT_DOUBLE_EQ(v, 2.0);
    ASSERT_TRUE(find_metric_value(out, "epoll_route_request_duration_seconds_bucket{" + base + ",le=\"+Inf\"} ", v));
    EXPECT_DOUBLE_EQ(v, 2.0);
}

TEST(MetricsRouteHistogram, FailureCountersBreakDownByCause) {
    auto& m = Metrics::instance();
    const std::string route = "__test_hist_b__";
    const std::string base = "route=\"" + route + "\",host=\"h\",tenant=\"t\"";

    m.record_route_request(route, "h", "t", 502, 0.01);  // upstream failure
    m.record_route_request(route, "h", "t", 401, 0.01);  // auth failure
    m.record_route_request(route, "h", "t", 429, 0.01);  // rate-limit rejection
    const std::string out = m.to_string();

    double v = 0.0;
    ASSERT_TRUE(find_metric_value(out, "epoll_route_upstream_errors_total{" + base + "} ", v));
    EXPECT_DOUBLE_EQ(v, 1.0);
    ASSERT_TRUE(find_metric_value(out, "epoll_route_auth_failures_total{" + base + "} ", v));
    EXPECT_DOUBLE_EQ(v, 1.0);
    ASSERT_TRUE(find_metric_value(out, "epoll_route_rate_limit_hits_total{" + base + "} ", v));
    EXPECT_DOUBLE_EQ(v, 1.0);
}

TEST(MetricsRouteHistogram, BucketsAreCumulative) {
    auto& m = Metrics::instance();
    const std::string route = "__test_hist_c__";
    const std::string base = "route=\"" + route + "\",host=\"h2\",tenant=\"t2\"";

    // Spread requests across the whole range; every bucket must be a running
    // total, i.e. non-decreasing as `le` grows, or histogram_quantile breaks.
    m.record_route_request(route, "h2", "t2", 200, 0.001);
    m.record_route_request(route, "h2", "t2", 200, 0.05);
    m.record_route_request(route, "h2", "t2", 200, 0.75);
    m.record_route_request(route, "h2", "t2", 200, 3.0);
    const std::string out = m.to_string();

    double previous = -1.0;
    const char* boundaries[] = {"0.005", "0.01", "0.025", "0.05", "0.1", "0.25",
                                "0.5", "1", "2.5", "5", "10", "+Inf"};
    for (const char* le : boundaries) {
        const std::string prefix =
            "epoll_route_request_duration_seconds_bucket{" + base + ",le=\"" + le + "\"} ";
        double v = 0.0;
        ASSERT_TRUE(find_metric_value(out, prefix, v)) << "missing bucket le=\"" << le << "\"";
        EXPECT_GE(v, previous) << "bucket le=\"" << le << "\" is smaller than the previous one";
        previous = v;
    }
    EXPECT_DOUBLE_EQ(previous, 4.0) << "the +Inf bucket must hold every request";
}