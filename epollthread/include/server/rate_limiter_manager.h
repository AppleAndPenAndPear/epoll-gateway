#pragma once
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>
#include <chrono>
#include "rate_limiter.h"
#include "config.h"   // 引入 RateLimitConfig

class RateLimiterManager {
public:
    RateLimiterManager() = default;

    // 默认限流配置：当 key 不存在时用它创建限流器
    explicit RateLimiterManager(const RateLimitConfig& default_config)
        : default_config_(default_config) {}

    void update_default_config(const RateLimitConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        default_config_ = config;
    }

    // 使用默认配置尝试获取令牌
    bool try_acquire(const std::string& key) {
        return try_acquire(key, default_config_);
    }

    // 使用指定配置尝试获取令牌，key 可以是 "ip:127.0.0.1" 或 "api_key:test-key-123"
    // config 用于创建新的限流器（如果 key 不存在）
    bool try_acquire(const std::string& key, const RateLimitConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
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

    // 清理超过 idle 时间未使用的限流器，防止内存泄漏
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
    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<RateLimiter>> limiters_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_used_;
    RateLimitConfig default_config_;
};