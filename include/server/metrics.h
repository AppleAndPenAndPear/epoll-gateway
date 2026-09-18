#pragma once
#include <atomic>
#include <array>
#include <string>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include "http_client.h"

class Metrics {
private:
    Metrics();

    struct RouteMetricSample {
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> requests_2xx{0};
        std::atomic<uint64_t> requests_3xx{0};
        std::atomic<uint64_t> requests_4xx{0};
        std::atomic<uint64_t> requests_5xx{0};
        std::atomic<uint64_t> timeout_errors{0};
        std::atomic<uint64_t> auth_failures{0};
        std::atomic<uint64_t> rate_limit_hits{0};
        std::atomic<double> duration_sum{0.0};
        std::atomic<uint64_t> duration_count{0};
    };

    std::array<double, 11> bucket_boundaries_;      // Latency bucket boundaries (seconds)
    mutable std::array<std::atomic<uint64_t>, 12> duration_buckets_{}; // Request counts per latency bucket; the last one is +Inf
    std::atomic<uint64_t> duration_count_{0};   // Total number of requests since server start
    std::atomic<double> duration_sum_{0.0};     // Sum of latencies (seconds)

    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> requests_2xx_{0};
    std::atomic<uint64_t> requests_3xx_{0};
    std::atomic<uint64_t> requests_4xx_{0};
    std::atomic<uint64_t> requests_5xx_{0};

    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> cache_misses_{0};
    std::atomic<uint64_t> fd_cache_hits_{0};
    std::atomic<uint64_t> fd_cache_misses_{0};

    // Gzip compression cache statistics
    std::atomic<uint64_t> gzip_cache_hits_{0};
    std::atomic<uint64_t> gzip_cache_misses_{0};
    std::array<std::atomic<uint64_t>, 8> upstream_error_counts_{};

    mutable std::mutex route_metrics_mutex_;
    std::unordered_map<std::string, RouteMetricSample> route_metrics_;
public:
    static Metrics& instance(){
        static Metrics inst;
        return inst;
    }

    // Record the total number of requests (parsed ones)
    void record_total_request();

    // Record global 2/3/4/5xx requests and their latency (seconds)
    void record_request(int status_code, double duration_seconds);

    // Record per-route requests and their latency (seconds)
    void record_route_request(const std::string& route_name,
                             const std::string& host,
                             const std::string& tenant,
                             int status_code,
                             double duration_seconds);

    // File cache statistics
    void record_cache_hit();
    void record_cache_miss();

    // FD cache statistics
    void record_fd_cache_hit();
    void record_fd_cache_miss();

    // Generate Prometheus text format
    std::string to_string() const;

    // Point-in-time copy of the global counters for the admin stats endpoint
    struct Snapshot {
        uint64_t total_requests = 0;
        uint64_t requests_2xx = 0;
        uint64_t requests_3xx = 0;
        uint64_t requests_4xx = 0;
        uint64_t requests_5xx = 0;
        double duration_sum = 0.0;
        uint64_t duration_count = 0;
    };
    Snapshot snapshot() const;

    void record_gzip_cache_hit();
    void record_gzip_cache_miss();
    void record_upstream_error(BackendError error);
};