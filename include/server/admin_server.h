#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include "config.h"
#include "upstream_manager.h"

struct ParsedAdminRequest;
class Socket;

// Out-of-band operations API (P4): a small blocking-accept HTTP server on its
// own listen address, separate from the data plane. Endpoints (all require a
// valid key via X-API-Key or Authorization: Bearer):
//   GET  /admin/stats      — request counters and latency totals
//   GET  /admin/upstreams  — per-backend health and circuit-breaker state
//   POST /admin/reload     — validate config.json, then trigger a runtime
//                            reload (workers apply it at safe checkpoints,
//                            same path as SIGHUP)
// Auth failures are logged as AUDIT lines. The listener is non-blocking with
// a 1s poll timeout so it stops promptly with the process stop flag.
class AdminServer {
public:
    AdminServer(const Config& config, std::shared_ptr<UpstreamManager> upstream_manager,
                const std::string& config_path);
    ~AdminServer();

    void start();              // Spawns the accept loop thread
    void wait();               // Joins the thread (after the stop flag is set)

private:
    void run();
    void handle_connection(int fd, const std::string& peer);
    bool authorize(const ParsedAdminRequest& req, std::string* err);
    std::string handle_stats();
    std::string handle_upstreams();
    std::pair<int, std::string> handle_reload();   // <http status, json body>

    Config config_;
    std::shared_ptr<UpstreamManager> upstream_manager_;
    std::string config_path_;
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
    int listen_fd_ = -1;
    std::thread thread_;
};

extern std::atomic<bool> stop_server_flag;
extern std::atomic<uint64_t> config_reload_generation;
