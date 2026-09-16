#pragma once
#include <atomic>
#include <chrono>
#include <mutex>

class RateLimiter {
public:
    RateLimiter(size_t capacity, size_t refill_per_second);

    // Try to acquire one token; returns true on success, false otherwise. Not thread-safe; the caller must hold an external lock
    bool try_acquire();

    size_t capacity() const { return capacity_; }
    size_t refill_per_second() const { return refill_per_second_; }
private:
    std::atomic<size_t> capacity_;
    std::atomic<size_t> refill_per_second_;     // Tokens refilled per second
    std::atomic<size_t> tokens_;        // Currently available tokens
    std::chrono::steady_clock::time_point last_refill_;    // Time of the last refill
    double tokens_fraction_ = 0.0;      // Fractional token accumulator to avoid truncation by static_cast
};