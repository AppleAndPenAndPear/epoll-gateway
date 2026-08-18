#pragma once
#include <atomic>
#include <chrono>
#include <mutex>

class RateLimiter {
public:
    RateLimiter(size_t capacity, size_t refill_per_second);

    // 尝试获取一个令牌，成功返回 true，失败返回 false; 非线程安全，调用方必须持有外部锁
    bool try_acquire();

    size_t capacity() const { return capacity_; }
    size_t refill_per_second() const { return refill_per_second_; }
private:
    std::atomic<size_t> capacity_;
    std::atomic<size_t> refill_per_second_;     //每秒补充的令牌数
    std::atomic<size_t> tokens_;        //当前可用令牌数
    std::chrono::steady_clock::time_point last_refill_;    //上次补充令牌的时间点
    double tokens_fraction_ = 0.0;      // 令牌补充的小数累积，避免 static_cast 截断丢失
};