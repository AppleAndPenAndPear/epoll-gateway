#include "server.h"
#include <sys/epoll.h>
#include "mylogger.h"
#include "spdlog/spdlog.h"
#include <csignal>
#include "config.h"

std::atomic<bool> stop_server_flag{false};
std::atomic<uint64_t> config_reload_generation{0};
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM)
        stop_server_flag.store(true);
    else if (signal == SIGHUP)
        config_reload_generation.fetch_add(1, std::memory_order_relaxed);
}

int main(){
    // 1. Initialize logging (must be called first)
    Logger::Guard g("logs/epollserver.log");
    auto logger = Logger::get();

    // Load the config file (falls back to defaults if missing), then validate it.
    // A config that fails validation aborts startup instead of silently running
    // on defaults (which could leave security policies unset).
    std::vector<std::string> config_errors;
    Config config = Config::from_file("config.json", &config_errors);
    for (const std::string& e : Config::validate(config)) {
        config_errors.push_back(e);
    }
    if (!config_errors.empty()) {
        for (const std::string& e : config_errors) {
            logger->error("Config validation failed: {}", e);
            std::cerr << "Config error: " << e << '\n';
        }
        return 1;
    }

    // TLS must come up at startup; abort on a broken or mismatched cert/key pair
    std::string tls_error;
    SSL_CTX* tls_probe = build_hardened_ssl_ctx(config.tls.cert_path, config.tls.key_path, &tls_error);
    if (!tls_probe) {
        logger->critical("TLS setup failed: {}", tls_error);
        std::cerr << "TLS error: " << tls_error << '\n';
        return 1;
    }
    SSL_CTX_free(tls_probe);

    // Security hint: secrets should live outside the main config file
    if (!config.api_keys.empty() && config.api_keys_file.empty()) {
        logger->warn("api_keys are defined inline in the main config; "
                     "prefer 'api_keys_file' or the GW_API_KEYS environment variable");
    }

    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    logger->info("Starting epoll server...");

    try {
        Tcpserver t(config.port, config.backlog,
                    config.thread_pool.min_threads,
                    config.thread_pool.max_threads,
                    config.thread_pool.scale_up_threshold,
                    config.thread_pool.scale_down_threshold);
        logger->info("Starting epoll server on port {}", config.port);
        t.start(config.num_workers, config, "config.json");
    } catch (const system_error& e) {
        logger->critical("Server startup failed: {}", e.what());
        std::cerr << "System error: " << e.what() << " [code: " << e.code() << "]\n";
        return 1;
    } catch (const exception& e) {
        logger->critical("Server startup failed: {}", e.what());
        std::cerr << "Standard exception: " << e.what() << '\n';
        return 1;
    } catch (...) {
        logger->critical("Server startup failed: unknown exception");
        std::cerr << "Unknown exception\n";
        return 1;
    }

    logger->info("Server stopped.");
    return 0;
}