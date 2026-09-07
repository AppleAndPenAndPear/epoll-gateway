#include "upstream_manager.h"
#include "mysocket.h"
#include "poller.h"
#include "mylogger.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdexcept>
#include <tuple>
#include <vector>

UpstreamManager::UpstreamManager(const UpstreamConfig& config)
    : config_(config), health_check_timeout_ms_(config.health_check_timeout_ms) {}

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