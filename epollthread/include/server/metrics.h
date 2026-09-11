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

    std::array<double, 11> bucket_boundaries_;      //延迟桶边界（秒）
    mutable std::array<std::atomic<uint64_t>, 12> duration_buckets_{}; // 最后一个为 +Inf，落入延迟桶的请求数
    std::atomic<uint64_t> duration_count_{0};   // 从服务器启动到当前时刻，所有请求的总数量
    std::atomic<double> duration_sum_{0.0};     // 延迟总和（秒）

    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> requests_2xx_{0};
    std::atomic<uint64_t> requests_3xx_{0};
    std::atomic<uint64_t> requests_4xx_{0};
    std::atomic<uint64_t> requests_5xx_{0};

    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> cache_misses_{0};
    std::atomic<uint64_t> fd_cache_hits_{0};
    std::atomic<uint64_t> fd_cache_misses_{0};

    // gzip 压缩缓存统计
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

    // 记录总请求，即解析出的
    void record_total_request();

    // 记录全局维度的2/3/4/5xx请求及其延迟（秒）
    void record_request(int status_code, double duration_seconds);

    //记录路由维度的请求及其延迟（秒）
    void record_route_request(const std::string& route_name,
                             const std::string& host,
                             const std::string& tenant,
                             int status_code,
                             double duration_seconds);

    // 文件缓存统计
    void record_cache_hit();
    void record_cache_miss();

    // FD 缓存统计
    void record_fd_cache_hit();
    void record_fd_cache_miss();

    // 生成 Prometheus 文本格式
    std::string to_string() const;

    void record_gzip_cache_hit();
    void record_gzip_cache_miss();
    void record_upstream_error(BackendError error);
};