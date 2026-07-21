#include "server.h"
#include <sys/epoll.h>
#include "mylogger.h"
#include "spdlog/spdlog.h"
#include <csignal>
#include "config.h"

std::atomic<bool> stop_server_flag{false};
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM)
        stop_server_flag.store(true);
}

int main(){
    // 1. 初始化日志（必须最先调用）
    Logger::Guard g("logs/epollserver.log");
    auto logger = Logger::get();

    // 加载配置文件（如果不存在则使用默认值）
    Config config = Config::from_file("config.json");

    // 注册信号
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    logger->info("Starting epoll server...");

    try {
        Tcpserver t(config.port, config.backlog,
                    config.thread_pool.min_threads,
                    config.thread_pool.max_threads,
                    config.thread_pool.scale_up_threshold,
                    config.thread_pool.scale_down_threshold);
        logger->info("Starting epoll server on port {}", config.port);
        t.start(config.num_workers,config.www_root,config.cache_max_entries,config.cache_max_file_size_mb,config.keepalive_timeout);
    } catch (const system_error& e) {
        logger->critical("Server startup failed: {}", e.what());
        std::cerr << "系统错误: " << e.what() << " [code: " << e.code() << "]\n";
        return 1;
    } catch (const exception& e) {
        logger->critical("Server startup failed: {}", e.what());
        std::cerr << "标准异常: " << e.what() << '\n';
        return 1;
    } catch (...) {
        logger->critical("Server startup failed: unknown exception");
        std::cerr << "未知异常\n";
        return 1;
    }

    logger->info("Server stopped.");
    return 0;
}