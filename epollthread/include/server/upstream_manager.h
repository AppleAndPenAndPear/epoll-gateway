#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include "config.h"   // 引入 UpstreamConfig, Upstream, UpstreamServer 等定义

class UpstreamManager {
private:
    UpstreamConfig config_;  // 拷贝一份配置，内部管理健康状态
    std::mutex mutex_;       // 保护 config_ 和轮询索引
    std::unordered_map<std::string, size_t> round_robin_indices_;
    int health_check_timeout_ms_;

    struct CircuitState {
        int consecutive_failures = 0;
        std::chrono::steady_clock::time_point opened_at{};
        bool open = false;      // false 表示 CLOSED：允许正常请求；true 表示 OPEN：暂时拒绝请求。
        bool half_open_probe = false;   // OPEN 超时后只允许一个请求做恢复探测，避免并发请求同时冲击故障后端。
    };
    std::unordered_map<std::string, CircuitState> circuit_states_;

    // 内部健康探测：TCP 连接测试
    bool health_probe(const std::string& host, int port) const;
public:
    explicit UpstreamManager(const UpstreamConfig& config);

    void reload(const UpstreamConfig& config);
    
    // 在真正连接后端前判断当前熔断状态是否允许本次请求通过。
    bool allow_request(const std::string& upstream_name,const UpstreamServer& server,int recovery_timeout_ms);
    void record_success(const std::string& upstream_name, const UpstreamServer& server);
    void record_failure(const std::string& upstream_name, const UpstreamServer& server,
                        int failure_threshold, int recovery_timeout_ms);

    // 根据上游服务名选择一个健康的后端服务器（轮询）
    const UpstreamServer& pick_server(const std::string& upstream_name);

    // 主动健康检查：对所有上游服务器的所有节点进行探测，更新健康状态
    void check_health();

    // 获取上游配置的只读访问（用于监控或调试）
    const UpstreamConfig& config() const { return config_; }
};