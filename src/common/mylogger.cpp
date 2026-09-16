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

            // production: the file sink logs warn and above to reduce disk I/O
            auto file_sink = make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file_path, 1024 * 1024 * 50, 5);  // 50MB, 5 files
            file_sink->set_level(spdlog::level::info);
            
            vector<spdlog::sink_ptr> sinks {console_sink, file_sink};

            // larger queue and 2 worker threads to reduce contention
            spdlog::init_thread_pool(32768, 2);
            
            // 3. create the async logger on the global thread pool
            auto logger = std::make_shared<spdlog::async_logger>(
                "server", sinks.begin(), sinks.end(),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::overrun_oldest);
            
            // production: level set to debug for easier troubleshooting
            logger->set_level(spdlog::level::debug);
            // flush immediately after every info-or-higher log so `tail -f` shows it in real time
            // (otherwise the async logger buffers content in the FILE* buffer and low-frequency messages only hit disk at process exit)
            logger->flush_on(spdlog::level::info);
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

            // 4. register and set as default
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
        // very early fallback (synchronous console logger)
        static auto fallback = spdlog::stdout_color_mt("fallback_sync");
        return fallback;
    }

    return logger;
}

void Logger::shutdown(){
     spdlog::shutdown();
}