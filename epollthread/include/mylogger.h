#pragma once
#include <memory>
#include <string>
#include "spdlog/spdlog.h"

using namespace std;
namespace spdlog {
    class logger;
    namespace sinks {
        class sink;
    }
}

class Logger {
public:
    // 禁止拷贝和移动
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 初始化日志系统（必须在 main 函数开始时调用）
    static void init(const string& log_file_path = "logs/server.log");

    // 获取全局 logger 实例
    static shared_ptr<spdlog::logger> get();

    // 关闭日志系统（通常在 main 结束前调用）
    static void shutdown();

    //引入守卫类，实现异常情况也能释放logger，以及记录必要日志
    class Guard {
    public:
        explicit Guard(const std::string& log_path = "logs/server.log") {
            Logger::init(log_path);
        }
        ~Guard() {
            Logger::shutdown();
        }
        // 禁止拷贝
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
private:
    Logger() = default;
};