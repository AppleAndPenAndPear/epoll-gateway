#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include "config.h"   // Brings in UpstreamConfig, Upstream, UpstreamServer, etc.

class UpstreamManager {
private:
    UpstreamConfig config_;  // Copy of the config; health state is managed internally
    std::mutex mutex_;       // Protects config_ and the round-robin indices
    std::unordered_map<std::string, size_t> round_robin_indices_;
    int health_check_timeout_ms_;

    struct CircuitState {
        int consecutive_failures = 0;
        std::chrono::steady_clock::time_point opened_at{};
        bool open = false;      // false = CLOSED: normal requests allowed; true = OPEN: requests temporarily rejected.
        bool half_open_probe = false;   // After OPEN times out, only one request may probe recovery, avoiding concurrent requests hammering the failing backend.
    };
    std::unordered_map<std::string, CircuitState> circuit_states_;

    // Internal health probe: TCP connection test
    bool health_probe(const std::string& host, int port) const;
public:
    explicit UpstreamManager(const UpstreamConfig& config);

    void reload(const UpstreamConfig& config);

    // Check whether the current circuit state allows the request before actually connecting to the backend.
    bool allow_request(const std::string& upstream_name,const UpstreamServer& server,int recovery_timeout_ms);
    void record_success(const std::string& upstream_name, const UpstreamServer& server);
    void record_failure(const std::string& upstream_name, const UpstreamServer& server,
                        int failure_threshold, int recovery_timeout_ms);

    // Pick a healthy backend server for the named upstream (round-robin)
    const UpstreamServer& pick_server(const std::string& upstream_name);

    // Active health check: probe every node of every upstream and update health state
    void check_health();

    // Read-only access to the upstream config (for monitoring or debugging)
    const UpstreamConfig& config() const { return config_; }
};