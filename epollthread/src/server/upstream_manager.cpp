#include "upstream_manager.h"
#include "mylogger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdexcept>

UpstreamManager::UpstreamManager(const UpstreamConfig& config) : config_(config) {}

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
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [name, upstream] : config_.upstreams) {
        for (auto& server : upstream.servers) {
            bool ok = health_probe(server.host, server.port);
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
}

bool UpstreamManager::health_probe(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    close(fd);
    return true;
}