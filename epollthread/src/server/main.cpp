#include "server.h"
#include <sys/epoll.h>
#include "mylogger.h"
#include "spdlog/spdlog.h"
#include <csignal>

std::atomic<bool> stop_server_flag{false};
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM)
        stop_server_flag.store(true);
}

int main(){
    // 1. 初始化日志（必须最先调用）
    Logger::Guard g("logs/epollserver.log");
    auto logger = Logger::get();

    // 注册信号
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    logger->info("Starting epoll server...");

    try {
        Tcpserver t(5005,1024,2,10,2,1);
        logger->info("epoll server is working, port = 5005");
        t.start();
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