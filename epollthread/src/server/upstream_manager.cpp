#include "upstream_manager.h"
#include "mysocket.h"
#include "poller.h"
#include "mylogger.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <algorithm>

UpstreamManager::UpstreamManager(const UpstreamConfig& config)
    : config_(config), health_check_timeout_ms_(config.health_check_timeout_ms) {}

namespace {
std::string circuit_key(const std::string& upstream_name, const UpstreamServer& server) {
    return upstream_name + "|" + server.host + ":" + std::to_string(server.port);
}
}

void UpstreamManager::reload(const UpstreamConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    health_check_timeout_ms_ = config.health_check_timeout_ms;
    round_robin_indices_.clear();
    circuit_states_.clear();
}

bool UpstreamManager::allow_request(const std::string& upstream_name,
                                    const UpstreamServer& server,
                                    int recovery_timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = circuit_states_[circuit_key(upstream_name, server)];

    // CLOSED: the failure threshold has not been reached; allow requests normally.
    if (!state.open) {
        return true;
    }

    // OPEN: the backend just failed repeatedly; enter the cooldown window first instead of amplifying the failure traffic.
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state.opened_at).count();

    // Reject if the cooldown is not over, or another request already holds the half-open probe slot.
    if (elapsed < recovery_timeout_ms || state.half_open_probe) {
        return false;
    }

    // HALF-OPEN: the cooldown is over; let exactly one probe request through, and record_success/failure decides the outcome.
    state.half_open_probe = true;
    return true;
}

void UpstreamManager::record_success(const std::string& upstream_name,
                                     const UpstreamServer& server) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = circuit_states_[circuit_key(upstream_name, server)];
    // A probe or normal request succeeded: reset the failure count, close the circuit, and resume normal traffic.
    state.consecutive_failures = 0;
    state.open = false;
    state.half_open_probe = false;
}

void UpstreamManager::record_failure(const std::string& upstream_name,
                                     const UpstreamServer& server,
                                     int failure_threshold,
                                     int recovery_timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = circuit_states_[circuit_key(upstream_name, server)];
    // Only a final failure (including all retries allowed for this request) counts toward the circuit failure count.
    state.half_open_probe = false;
    state.consecutive_failures++;
    if (state.consecutive_failures >= std::max(1, failure_threshold)) {
        // OPEN: threshold reached, record the open time; subsequent requests enter the recovery wait window.
        state.open = true;
        state.opened_at = std::chrono::steady_clock::now();
        Logger::get()->warn("Circuit opened for upstream {} server {}:{} for {} ms after {} failures",
                            upstream_name, server.host, server.port,
                            recovery_timeout_ms, state.consecutive_failures);
    }
}

const UpstreamServer& UpstreamManager::pick_server(const std::string& upstream_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = config_.upstreams.find(upstream_name);
    if (it == config_.upstreams.end() || it->second.servers.empty()) {
        throw std::runtime_error("No upstream found or empty: " + upstream_name);
    }

    auto& servers = it->second.servers;
    size_t size = servers.size();
    size_t idx = round_robin_indices_[upstream_name]++ % size;

    // Find the first healthy server starting from idx
    for (size_t i = 0; i < size; ++i) {
        size_t candidate = (idx + i) % size;
        if (servers[candidate].healthy) {
            return servers[candidate];
        }
    }

    // All servers are unhealthy; still return the one at idx (the caller decides how to handle it)
    return servers[idx];
}

void UpstreamManager::check_health() {
    struct ProbeTarget {
        std::string upstream_name;
        size_t server_index;
        std::string host;
        int port;
    };

    std::vector<ProbeTarget> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, upstream] : config_.upstreams) {
            for (size_t index = 0; index < upstream.servers.size(); ++index) {
                const auto& server = upstream.servers[index];
                targets.push_back({name, index, server.host, server.port});
            }
        }
    }

    std::vector<std::tuple<std::string, size_t, bool>> results;
    results.reserve(targets.size());
    for (const auto& target : targets) {
        results.emplace_back(target.upstream_name, target.server_index,
                             health_probe(target.host, target.port));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, index, ok] : results) {
        auto upstream_it = config_.upstreams.find(name);
        if (upstream_it == config_.upstreams.end() || index >= upstream_it->second.servers.size()) {
            continue;
        }
        auto& server = upstream_it->second.servers[index];
        if (!ok && server.healthy) {
            Logger::get()->warn("Upstream {} server {}:{} is down", name, server.host, server.port);
            server.healthy = false;
            server.consecutive_failures++;
        } else if (ok && !server.healthy) {
            Logger::get()->info("Upstream {} server {}:{} is back online", name, server.host, server.port);
            server.healthy = true;
            server.consecutive_failures = 0;
        } else if (!ok) {
            server.consecutive_failures++;
        } else {
            server.consecutive_failures = 0;
        }
    }
}

bool UpstreamManager::health_probe(const std::string& host, int port) const {
    try {
        Socket socket(AF_INET, SOCK_STREAM, 0);
        socket.setnonblocking();

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            return false;
        }

        if (socket.connect(reinterpret_cast<const sockaddr*>(&address), sizeof(address))) {
            return true;
        }

        const auto result = Poller::wait(socket.getFd(), Poller::Event::Write,
                                         health_check_timeout_ms_);
        return result == Poller::WaitResult::Ready && socket.socketError() == 0;
    } catch (const std::exception&) {
        return false;
    }
}