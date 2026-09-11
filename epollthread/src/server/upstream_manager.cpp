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

    // CLOSED：当前没有达到失败阈值，正常放行请求。
    if (!state.open) {
        return true;
    }

    // OPEN：后端刚刚连续失败，先进入冷却窗口，不继续放大故障流量。
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state.opened_at).count();

    // 冷却未结束，或者已有其他请求占用了半开探测名额，直接拒绝。
    if (elapsed < recovery_timeout_ms || state.half_open_probe) {
        return false;
    }

    // HALF-OPEN：冷却结束，只放行一个探测请求；结果由 record_success/failure 决定。
    state.half_open_probe = true;
    return true;
}

void UpstreamManager::record_success(const std::string& upstream_name,
                                     const UpstreamServer& server) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = circuit_states_[circuit_key(upstream_name, server)];
    // 探测或普通请求成功，清空失败计数并关闭熔断，恢复正常放行。
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
    // 最终失败结果（包括本次请求允许的重试都失败）才会累计熔断失败次数。
    state.half_open_probe = false;
    state.consecutive_failures++;
    if (state.consecutive_failures >= std::max(1, failure_threshold)) {
        // OPEN：达到阈值，记录打开时间，后续请求进入恢复等待窗口。
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

    // 从 idx 开始寻找第一个健康的服务器
    for (size_t i = 0; i < size; ++i) {
        size_t candidate = (idx + i) % size;
        if (servers[candidate].healthy) {
            return servers[candidate];
        }
    }

    // 所有服务器都不健康，仍然返回 idx 对应的（由调用者决定如何处理）
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