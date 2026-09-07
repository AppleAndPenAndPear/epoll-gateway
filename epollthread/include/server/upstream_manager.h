#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "config.h"   // 引入 UpstreamConfig, Upstream, UpstreamServer 等定义

class UpstreamManager {
private:
    UpstreamConfig config_;  // 拷贝一份配置，内部管理健康状态
    std::mutex mutex_;       // 保护 config_ 和轮询索引
    std::unordered_map<std::string, size_t> round_robin_indices_;
    int health_check_timeout_ms_;

    // 内部健康探测：TCP 连接测试
    bool health_probe(const std::string& host, int port) const;
public:
    explicit UpstreamManager(const UpstreamConfig& config);

    // 根据上游服务名选择一个健康的后端服务器（轮询）
    const UpstreamServer& pick_server(const std::string& upstream_name);

    // 主动健康检查：对所有上游服务器的所有节点进行探测，更新健康状态
    void check_health();

    // 获取上游配置的只读访问（用于监控或调试）
    const UpstreamConfig& config() const { return config_; }
};