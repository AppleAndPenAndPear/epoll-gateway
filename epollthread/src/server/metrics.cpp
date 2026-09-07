#include "metrics.h"

Metrics::Metrics() {
    // 初始化桶边界（秒）
    bucket_boundaries_ = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
    for (auto& b : duration_buckets_) b.store(0);
}

void Metrics::record_total_request() {
    total_requests_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_request(int status_code, double duration_seconds) {
    if (status_code >= 200 && status_code < 300) requests_2xx_.fetch_add(1, std::memory_order_relaxed);
    else if (status_code >= 300 && status_code < 400) requests_3xx_.fetch_add(1, std::memory_order_relaxed);
    else if (status_code >= 400 && status_code < 500) requests_4xx_.fetch_add(1, std::memory_order_relaxed);
    else if (status_code >= 500) requests_5xx_.fetch_add(1, std::memory_order_relaxed);
        
    // std::atomic<double> 不支持 fetch_add，使用 CAS 循环
    double old_sum = duration_sum_.load(std::memory_order_relaxed);
    while (!duration_sum_.compare_exchange_weak(old_sum, old_sum + duration_seconds,std::memory_order_relaxed, std::memory_order_relaxed)) {}
    duration_count_.fetch_add(1, std::memory_order_relaxed);
    // 更新直方图桶
    for (size_t i = 0; i < bucket_boundaries_.size(); ++i) {
        if (duration_seconds <= bucket_boundaries_[i]) {
            duration_buckets_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    // +Inf 桶（所有请求）
    duration_buckets_[bucket_boundaries_.size()].fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_cache_hit() {
    cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_cache_miss() {
    cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_fd_cache_hit() {
    fd_cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_fd_cache_miss() {
    fd_cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_gzip_cache_hit()   { 
    gzip_cache_hits_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::record_gzip_cache_miss()  { 
    gzip_cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_upstream_error(BackendError error) {
    if (error == BackendError::None) {
        return;
    }
    const auto index = static_cast<size_t>(error);
    if (index < upstream_error_counts_.size()) {
        upstream_error_counts_[index].fetch_add(1, std::memory_order_relaxed);
    }
}

std::string Metrics::to_string() const {
        std::ostringstream oss;
        // 总请求
        oss << "# HELP epoll_http_requests_total Total number of HTTP requests\n";
        oss << "# TYPE epoll_http_requests_total counter\n";
        oss << "epoll_http_requests_total{code=\"2xx\"} " << requests_2xx_.load() << "\n";
        oss << "epoll_http_requests_total{code=\"3xx\"} " << requests_3xx_.load() << "\n";
        oss << "epoll_http_requests_total{code=\"4xx\"} " << requests_4xx_.load() << "\n";
        oss << "epoll_http_requests_total{code=\"5xx\"} " << requests_5xx_.load() << "\n";
        oss << "epoll_http_requests_total_total " << total_requests_.load() << "\n";

        // 延迟直方图
        oss << "# HELP epoll_http_request_duration_seconds Request duration in seconds\n";
        oss << "# TYPE epoll_http_request_duration_seconds histogram\n";
        for (size_t i = 0; i < bucket_boundaries_.size(); ++i) {
            oss << "epoll_http_request_duration_seconds_bucket{le=\"" << bucket_boundaries_[i] << "\"} "
                << duration_buckets_[i].load() << "\n";
        }
        oss << "epoll_http_request_duration_seconds_bucket{le=\"+Inf\"} "
            << duration_buckets_[bucket_boundaries_.size()].load() << "\n";
        oss << "epoll_http_request_duration_seconds_sum " << duration_sum_.load() << "\n";
        oss << "epoll_http_request_duration_seconds_count " << duration_count_.load() << "\n";

        // 缓存命中率
        oss << "# HELP epoll_cache_hits_total Total cache hits\n";
        oss << "# TYPE epoll_cache_hits_total counter\n";
        oss << "epoll_cache_hits_total " << cache_hits_.load() << "\n";
        oss << "# HELP epoll_cache_misses_total Total cache misses\n";
        oss << "# TYPE epoll_cache_misses_total counter\n";
        oss << "epoll_cache_misses_total " << cache_misses_.load() << "\n";

        // FD 缓存命中率
        oss << "# HELP epoll_fd_cache_hits_total Total fd cache hits\n";
        oss << "# TYPE epoll_fd_cache_hits_total counter\n";
        oss << "epoll_fd_cache_hits_total " << fd_cache_hits_.load() << "\n";
        oss << "# HELP epoll_fd_cache_misses_total Total fd cache misses\n";
        oss << "# TYPE epoll_fd_cache_misses_total counter\n";
        oss << "epoll_fd_cache_misses_total " << fd_cache_misses_.load() << "\n";

        // Gzip 缓存命中率
        oss << "# HELP epoll_gzip_cache_hits_total Total gzip cache hits\n";
        oss << "# TYPE epoll_gzip_cache_hits_total counter\n";
        oss << "epoll_gzip_cache_hits_total " << gzip_cache_hits_.load() << "\n";
        oss << "# HELP epoll_gzip_cache_misses_total Total gzip cache misses\n";
        oss << "# TYPE epoll_gzip_cache_misses_total counter\n";
        oss << "epoll_gzip_cache_misses_total " << gzip_cache_misses_.load() << "\n";

        oss << "# HELP epoll_upstream_errors_total Upstream proxy errors by type\n";
        oss << "# TYPE epoll_upstream_errors_total counter\n";
        const std::array<const char*, 8> error_names = {
            "none", "connect_failed", "connect_timeout", "write_failed",
            "write_timeout", "read_failed", "read_timeout", "invalid_response"
        };
        for (size_t i = 1; i < error_names.size(); ++i) {
            oss << "epoll_upstream_errors_total{type=\"" << error_names[i] << "\"} "
                << upstream_error_counts_[i].load() << "\n";
        }

        oss << "# EOF\n";
        return oss.str();
    }