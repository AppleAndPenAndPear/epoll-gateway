#include "connection_pool.h"

ConnectionPool& global_connection_pool() {
    static ConnectionPool pool;
    return pool;
}

namespace {
// The scheme is part of the identity: the same host:port must never hand back
// a plaintext connection for an https upstream (or vice versa), nor a session
// verified for one server name to an upstream verifying another.
std::string pool_key(const std::string& host, int port, const std::string& scheme) {
    return scheme + host + ":" + std::to_string(port);
}
}  // namespace

std::optional<ConnectionPool::PooledConnection> ConnectionPool::checkout(const std::string& host,
                                                                         int port,
                                                                         const std::string& scheme) {
    const std::string key = pool_key(host, port, scheme);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = idle_.find(key);
    if (it == idle_.end()) return std::nullopt;

    const auto expiry = std::chrono::steady_clock::now() - effective_idle_timeout();
    while (!it->second.empty()) {
        PooledConnection conn = std::move(it->second.front());
        it->second.pop_front();
        if (conn.last_used < expiry) {
            continue;  // expired; drop (closing happens via the shared_ptr)
        }
        if (it->second.empty()) idle_.erase(it);
        return conn;
    }
    idle_.erase(it);
    return std::nullopt;
}

void ConnectionPool::checkin(const std::string& host, int port, const std::string& scheme,
                             PooledConnection conn) {
    const std::string key = pool_key(host, port, scheme);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& dq = idle_[key];
    if (dq.size() >= max_idle_per_upstream()) {
        return;  // pool full; the connection closes when the shared_ptr dies
    }
    conn.last_used = std::chrono::steady_clock::now();
    dq.push_back(std::move(conn));
}

void ConnectionPool::invalidate(const std::string& host, int port, const std::string& scheme) {
    const std::string key = pool_key(host, port, scheme);
    std::lock_guard<std::mutex> lock(mutex_);
    idle_.erase(key);
}
