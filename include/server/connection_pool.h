#pragma once
#include "mysocket.h"
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// Per-upstream pool of idle keep-alive connections, shared by all worker
// threads. The gateway previously opened one TCP connection per proxied
// request; pooling removes the repeated three-way handshake and lets the
// backend amortize accept/connection overhead.
class ConnectionPool {
public:
    struct PooledConnection {
        std::shared_ptr<Socket> socket;
        // Bytes read past the end of the previous response (must be consumed
        // before the next response on this connection).
        std::string leftover;
        std::chrono::steady_clock::time_point last_used;
    };

    // Returns an idle connection for host:port, or nullopt when none is
    // available. Expired entries are evicted lazily on checkout.
    std::optional<PooledConnection> checkout(const std::string& host, int port);

    // Returns a healthy keep-alive connection to the pool. Anything beyond
    // max_idle_per_upstream() for that upstream is dropped (closed).
    void checkin(const std::string& host, int port, PooledConnection conn);

    // Drops a connection that is suspected broken (send/read error, peer close).
    void invalidate(const std::string& host, int port);

    static constexpr size_t max_idle_per_upstream() { return 16; }
    static constexpr std::chrono::seconds idle_timeout() { return std::chrono::seconds(60); }

    // Test hook: shrink the idle timeout so eviction can be verified quickly.
    void set_idle_timeout_for_testing(std::chrono::seconds t) {
        std::lock_guard<std::mutex> lock(mutex_);
        test_idle_timeout_ = t;
    }

private:
    std::chrono::seconds effective_idle_timeout() const {
        return test_idle_timeout_.value_or(idle_timeout());
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::deque<PooledConnection>> idle_;
    std::optional<std::chrono::seconds> test_idle_timeout_;
};

// Process-wide pool shared by all workers.
ConnectionPool& global_connection_pool();
