#include "rate_limiter.h"
#include <algorithm>

RateLimiter::RateLimiter(size_t capacity, size_t refill_per_second)
        : capacity_(capacity), refill_per_second_(refill_per_second),tokens_(capacity), last_refill_(std::chrono::steady_clock::now()) {}

bool RateLimiter::try_acquire() {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;

    // Refill tokens: accumulate the fractional part so token refills are not lost to static_cast truncation
    double new_tokens = elapsed * refill_per_second_ + tokens_fraction_;
    size_t whole = static_cast<size_t>(new_tokens);
    tokens_fraction_ = new_tokens - static_cast<double>(whole);
    if (whole > 0) {
        tokens_ = std::min(capacity_.load(), tokens_.load() + whole);
    }

    if (tokens_.load() > 0) {
        tokens_.fetch_sub(1);
        return true;
    }
    return false;
}