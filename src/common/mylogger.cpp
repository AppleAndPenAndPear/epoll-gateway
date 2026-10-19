#include "mylogger.h"
#include <atomic>
#include <thread>
#include <chrono>
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

            // Synchronous logger. The async logger added a per-message
            // cross-thread handoff (condvar signal + wake, plus a heap copy of
            // every formatted line into the queue), two worker threads and a
            // periodic flusher per worker process; profiling under c50 probe
            // load attributed ~11% of process CPU to that logging thread for
            // one access-log line per request. Writing synchronously to the
            // buffered file sink costs a memcpy into the stdio buffer (the 1 s
            // periodic flush below bounds actual file I/O), which is how nginx
            // writes its access lines. The flush policy is unchanged.
            auto logger = std::make_shared<spdlog::logger>(
                "server", sinks.begin(), sinks.end());
            
            // production: level set to debug for easier troubleshooting
            logger->set_level(spdlog::level::debug);
            // Flush policy: flushing on every info message put an fflush+write
            // syscall on the request hot path (AUDIT + access log are info, so
            // every proxied request paid two disk flushes and dominated P50
            // latency; see BENCHMARKS.md). Errors and above still flush
            // immediately; info/warn content is flushed by the periodic
            // flusher below, so `tail -f` stays near-real-time and graceful
            // shutdown still flushes everything via spdlog::shutdown(). A hard
            // crash can lose at most one flush interval of info-level logs.
            logger->flush_on(spdlog::level::err);
            spdlog::flush_every(std::chrono::seconds(1));
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