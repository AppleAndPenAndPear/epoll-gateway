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
    // Non-copyable and non-movable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Initialize the logging system (must be called at the start of main)
    static void init(const string& log_file_path = "logs/server.log");

    // Get the global logger instance
    static shared_ptr<spdlog::logger> get();

    // Shut down the logging system (usually called before main returns)
    static void shutdown();

    // Guard class: releases the logger even when exceptions occur, and makes sure final logs are written
    class Guard {
    public:
        explicit Guard(const std::string& log_path = "logs/server.log") {
            Logger::init(log_path);
        }
        ~Guard() {
            Logger::shutdown();
        }
        // Non-copyable
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
private:
    Logger() = default;
};