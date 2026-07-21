#pragma once
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

struct Config {
    // 服务器端口
    unsigned short port = 5005;
    // listen backlog
    int backlog = 1024;
    // Worker 线程数（0 表示使用 hardware_concurrency）
    unsigned int num_workers = 0;
    // 静态文件根目录
    std::string www_root = "./www";

    size_t cache_max_entries = 1024;  // 线程内缓存最大条目数
    size_t cache_max_file_size_mb = 1; // 可缓存的最大文件大小（MB）

    int keepalive_timeout = 60; // keep-alive 超时时间（秒）
    // 动态线程池配置
    struct ThreadPoolConfig {
        size_t min_threads = 2;
        size_t max_threads = 10;
        size_t scale_up_threshold = 2;
        size_t scale_down_threshold = 1;
    } thread_pool;

    // 从 JSON 文件加载配置，如果文件不存在或解析失败，保持默认值
    static Config from_file(const std::string& path) {
        Config config;
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            // 文件不存在，使用默认值
            return config;
        }
        nlohmann::json j;
        try {
            ifs >> j;
        } catch (...) {
            // 解析失败，使用默认值
            return config;
        }
        // 逐字段读取，如果存在就覆盖默认值
        if (j.contains("port")) config.port = j["port"];
        if (j.contains("backlog")) config.backlog = j["backlog"];
        if (j.contains("num_workers")) config.num_workers = j["num_workers"];
        if (j.contains("www_root")) config.www_root = j["www_root"];

        if (j.contains("thread_pool")) {
            auto& tp = j["thread_pool"];
            if (tp.contains("min")) config.thread_pool.min_threads = tp["min"];
            if (tp.contains("max")) config.thread_pool.max_threads = tp["max"];
            if (tp.contains("scale_up")) config.thread_pool.scale_up_threshold = tp["scale_up"];
            if (tp.contains("scale_down")) config.thread_pool.scale_down_threshold = tp["scale_down"];
        }
        if (j.contains("cache_max_entries")) {
            config.cache_max_entries = j["cache_max_entries"];
        }
        if (j.contains("cache_max_file_size_mb")) {
            config.cache_max_file_size_mb = j["cache_max_file_size_mb"];
        }
        if (j.contains("keepalive_timeout")) {
            config.keepalive_timeout = j["keepalive_timeout"];
        }
        return config;
    }
};