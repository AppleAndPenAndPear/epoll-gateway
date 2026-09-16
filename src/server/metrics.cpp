#include "metrics.h"

Metrics::Metrics() {
    // Initialize bucket boundaries (seconds)
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

    // std::atomic<double> does not support fetch_add; use a CAS loop
    double old_sum = duration_sum_.load(std::memory_order_relaxed);
    while (!duration_sum_.compare_exchange_weak(old_sum, old_sum + duration_seconds,std::memory_order_relaxed, std::memory_order_relaxed)) {}
    duration_count_.fetch_add(1, std::memory_order_relaxed);
    // Update histogram buckets
    for (size_t i = 0; i < bucket_boundaries_.size(); ++i) {
        if (duration_seconds <= bucket_boundaries_[i]) {
            duration_buckets_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    // +Inf bucket (all requests)
    duration_buckets_[bucket_boundaries_.size()].fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_route_request(const std::string& route_name,
                                  const std::string& host,
                                  const std::string& tenant,
                                  int status_code,
                                  double duration_seconds) {
    std::string tag = route_name + "|" + host + "|" + tenant;
    std::lock_guard<std::mutex> lock(route_metrics_mutex_);
    auto& sample = route_metrics_[tag];
    sample.total_requests.fetch_add(1, std::memory_order_relaxed);
    if (status_code >= 200 && status_code < 300) sample.requests_2xx.fetch_add(1, std::memory_order_relaxed);
    else if (status_code >= 300 && status_code < 400) sample.requests_3xx.fetch_add(1, std::memory_order_relaxed);
    else if (status_code >= 400 && status_code < 500) sample.requests_4xx.fetch_add(1, std::memory_order_relaxed);
    else if (status_code >= 500) sample.requests_5xx.fetch_add(1, std::memory_order_relaxed);
    if (status_code == 504 || status_code == 503 || status_code == 502) {
        sample.timeout_errors.fetch_add(1, std::memory_order_relaxed);
    }
    if (status_code == 401) {
        sample.auth_failures.fetch_add(1, std::memory_order_relaxed);
    }
    if (status_code == 429) {
        sample.rate_limit_hits.fetch_add(1, std::memory_order_relaxed);
    }
    double old_sum = sample.duration_sum.load(std::memory_order_relaxed);
    while (!sample.duration_sum.compare_exchange_weak(old_sum,
                                                     old_sum + duration_seconds,
                                                     std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {
    }
    sample.duration_count.fetch_add(1, std::memory_order_relaxed);
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
    std::lock_guard<std::mutex> lock(route_metrics_mutex_);
        // Total requests
        oss << "# HELP epoll_http_requests_total Total number of HTTP requests\n";
        oss << "# TYPE epoll_http_requests_total counter\n";
        oss << "epoll_http_requests_total{code=\"2xx\"} " << requests_2xx_.load() << "\n";
        oss << "epoll_http_requests_total{code=\"3xx\"} " << requests_3xx_.load() << "\n";
        oss << "epoll_http_requests_total{code=\"4xx\"} " << requests_4xx_.load() << "\n";
        oss << "epoll_http_requests_total{code=\"5xx\"} " << requests_5xx_.load() << "\n";
        oss << "epoll_http_requests_total_total " << total_requests_.load() << "\n";

        // Latency histogram
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

        // Cache hit rate
        oss << "# HELP epoll_cache_hits_total Total cache hits\n";
        oss << "# TYPE epoll_cache_hits_total counter\n";
        oss << "epoll_cache_hits_total " << cache_hits_.load() << "\n";
        oss << "# HELP epoll_cache_misses_total Total cache misses\n";
        oss << "# TYPE epoll_cache_misses_total counter\n";
        oss << "epoll_cache_misses_total " << cache_misses_.load() << "\n";

        // FD cache hit rate
        oss << "# HELP epoll_fd_cache_hits_total Total fd cache hits\n";
        oss << "# TYPE epoll_fd_cache_hits_total counter\n";
        oss << "epoll_fd_cache_hits_total " << fd_cache_hits_.load() << "\n";
        oss << "# HELP epoll_fd_cache_misses_total Total fd cache misses\n";
        oss << "# TYPE epoll_fd_cache_misses_total counter\n";
        oss << "epoll_fd_cache_misses_total " << fd_cache_misses_.load() << "\n";

        // Gzip cache hit rate
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

        oss << "# HELP epoll_route_requests_total Route-level request totals by route, host, tenant\n";
        oss << "# TYPE epoll_route_requests_total counter\n";
        for (const auto& [key, sample] : route_metrics_) {
            std::string route = key;
            auto sep1 = route.find('|');
            auto sep2 = route.rfind('|');
            std::string route_name = sep1 == std::string::npos ? route : route.substr(0, sep1);
            std::string host = sep1 == std::string::npos ? "-" : route.substr(sep1 + 1, sep2 - sep1 - 1);
            std::string tenant = sep2 == std::string::npos ? "-" : route.substr(sep2 + 1);
            oss << "epoll_route_requests_total{route=\"" << route_name << "\",host=\"" << host << "\",tenant=\"" << tenant << "\",code=\"2xx\"} "
                << sample.requests_2xx.load() << "\n";
            oss << "epoll_route_requests_total{route=\"" << route_name << "\",host=\"" << host << "\",tenant=\"" << tenant << "\",code=\"4xx\"} "
                << sample.requests_4xx.load() << "\n";
            oss << "epoll_route_requests_total{route=\"" << route_name << "\",host=\"" << host << "\",tenant=\"" << tenant << "\",code=\"5xx\"} "
                << sample.requests_5xx.load() << "\n";
            oss << "epoll_route_latency_seconds_sum{route=\"" << route_name << "\",host=\"" << host << "\",tenant=\"" << tenant << "\"} "
                << sample.duration_sum.load() << "\n";
            oss << "epoll_route_latency_seconds_count{route=\"" << route_name << "\",host=\"" << host << "\",tenant=\"" << tenant << "\"} "
                << sample.duration_count.load() << "\n";
        }

        oss << "# EOF\n";
        return oss.str();
    }