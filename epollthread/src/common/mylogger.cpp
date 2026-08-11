#include "mylogger.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <vector>
#include <iostream>

static std::once_flag init_flag;

void Logger::init(const string& log_file_path){
    std::call_once(init_flag, [&]() {
        try {
            auto console_sink = make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(spdlog::level::warn);

            // 生产环境：文件只记录 warn 及以上，减少磁盘 I/O
            auto file_sink = make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file_path, 1024 * 1024 * 50, 5);  // 50MB, 5个文件
            file_sink->set_level(spdlog::level::info);
            
            vector<spdlog::sink_ptr> sinks {console_sink, file_sink};

            // 队列加大，线程加到2，减少争用
            spdlog::init_thread_pool(32768, 2);
            
            // 3. 创建异步 logger，使用全局线程池
            auto logger = std::make_shared<spdlog::async_logger>(
                "server", sinks.begin(), sinks.end(),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::overrun_oldest);
            
            //生产环境：日志级别设置为 debug，方便调试
            logger->set_level(spdlog::level::debug);
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

            // 4. 注册并设为默认
            spdlog::register_logger(logger);
            spdlog::set_default_logger(logger);
        }
        catch (const spdlog::spdlog_ex& ex) {
            cerr << "Logger initialization failed: " << ex.what() << endl;
            throw;
        }
    });
}

shared_ptr<spdlog::logger> Logger::get(){
    auto logger = spdlog::get("server");

    if (!logger) {
        // 极早期 fallback（同步控制台 logger)
        static auto fallback = spdlog::stdout_color_mt("fallback_sync");
        return fallback;
    }

    return logger;
}

void Logger::shutdown(){
     spdlog::shutdown();
}