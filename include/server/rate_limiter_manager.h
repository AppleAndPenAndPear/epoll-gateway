#pragma once
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>
#include <chrono>
#include "rate_limiter.h"
#include "config.h"   // Brings in RateLimitConfig

class RateLimiterManager {
public:
    RateLimiterManager() = default;

    // Default rate limit config: used to create a limiter when the key has none
    explicit RateLimiterManager(const RateLimitConfig& default_config)
        : default_config_(default_config) {}

    void update_default_config(const RateLimitConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        default_config_ = config;
    }

    // Try to acquire a token using the default config. The default is read
    // under the same lock that update_default_config() writes it, so a SIGHUP
    // reload cannot race with this read.
    bool try_acquire(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return try_acquire_locked(key, default_config_);
    }

    // Try to acquire a token with the given config; key can be "ip:127.0.0.1" or "api_key:test-key-123"
    // config is used to create a new limiter (if the key does not exist yet)
    bool try_acquire(const std::string& key, const RateLimitConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        return try_acquire_locked(key, config);
    }

    // Remove limiters unused beyond the idle time to prevent memory leaks
    void cleanup(std::chrono::seconds idle = std::chrono::seconds(600)) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = limiters_.begin(); it != limiters_.end(); ) {
            auto last = last_used_.find(it->first);
            if (last != last_used_.end() && now - last->second > idle) {
                last_used_.erase(last);
                it = limiters_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    // Caller must hold mutex_.
    bool try_acquire_locked(const std::string& key, const RateLimitConfig& config) {
        auto it = limiters_.find(key);
        if (it == limiters_.end()) {
            it = limiters_.emplace(
                key,
                std::make_unique<RateLimiter>(config.capacity, config.refill_per_second)
            ).first;
        }
        last_used_[key] = std::chrono::steady_clock::now();
        return it->second->try_acquire();
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<RateLimiter>> limiters_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_used_;
    RateLimitConfig default_config_;
};