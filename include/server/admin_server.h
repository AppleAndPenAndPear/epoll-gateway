#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "config.h"
#include "mysocket.h"
#include "upstream_manager.h"

struct ParsedAdminRequest;

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
// `admin.api_keys` is hot-reloadable: after any accepted reload (SIGHUP or the
// admin API) the listener adopts the new keys within ~1 second, so key rotation
// needs no restart. `enabled`/`port`/`bind` are fixed at startup.
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
    void maybe_refresh_keys();   // Hot-reload admin.api_keys when a reload generation change is observed
    void apply_admin_keys(const std::vector<std::string>& keys, bool enabled, bool from_admin_api);

    // Admin key material from the config validated by the last accepted
    // POST /admin/reload. Keeping it avoids re-reading config.json during the
    // refresh — which would also risk adopting keys from a newer file revision
    // than the one that was validated. Unset when no method reload happened.
    struct PendingAdminKeys {
        bool enabled = false;
        std::vector<std::string> keys;
    };
    std::optional<PendingAdminKeys> pending_admin_keys_;

    Config config_;
    uint64_t applied_generation_ = 0;   // last reload generation we applied
    // Note: no locking needed — the accept loop handles connections inline, so
    // maybe_refresh_keys() and authorize() always run on the same thread.
    std::shared_ptr<UpstreamManager> upstream_manager_;
    std::string config_path_;
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
    Socket listen_sock_{-1};   // RAII fd owner; blocking I/O stays on ::recv/::send
    std::thread thread_;
};

extern std::atomic<bool> stop_server_flag;
extern std::atomic<uint64_t> config_reload_generation;
