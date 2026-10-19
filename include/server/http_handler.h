#pragma once
#include "http_parser.h"
#include "mysocket.h"
#include "myepoll.h"
#include <deque>
#include <vector>
#include <unordered_map>
#include <sys/types.h>     // needed for off_t; unistd.h also works
#include "file_cache.h"
#include <functional>
#include <map>
#include <mutex>
#include <unordered_set>
#include "fd_cache.h"
#include "config.h"
#include "upstream_manager.h"
#include "rate_limiter.h"
#include "rate_limiter_manager.h"
#include "api_key_manager.h"
#include "http_client.h"

class HttpHandler {
private:
    using RouteParams = std::map<std::string, std::string>;
    using RouteHandler = std::function<void(const HttpRequest&, HttpResponse&, const RouteParams&)>;

    struct RegisteredRoute {
        GatewayRoute route;
        RouteHandler handler;
    };

    struct ResolvedRoute {
        const RegisteredRoute* route = nullptr;
        RouteParams params;
        // True when path, Host and Tenant matched but the HTTP method did not; used to return 405.
        bool path_matched = false;
        std::string allow_methods;
    };

    // One backend round trip driven by this worker's event loop. A connection
    // holds one per in-flight backend call: exactly one for a single-upstream
    // route, one per dependency for a composite route. `fd` is the upstream
    // socket currently registered in the epoll set, or -1 when unregistered.
    struct ChildExchange {
        std::unique_ptr<UpstreamExchange> ex;
        UpstreamExchange::Params params;
        int fd = -1;
        UpstreamServer server;
        std::string upstream_key;
        int max_retries = 0;
        int circuit_failure_threshold = 0;
        int circuit_recovery_timeout_ms = 0;
        int attempt_count = 0;
        // Composite-only bookkeeping.
        bool finished = false;
        bool required = true;
        std::string name;               // dependency name: aggregate key + log label
        nlohmann::json item;            // this dependency's entry in the aggregate
    };

    // All backend round trips outstanding for one client connection. Single and
    // composite routes share this state and the same driver; composite routes
    // additionally enforce a hard overall deadline. Everything an exchange
    // touches is snapshotted by value: `config_` and `routes_` are reassigned in
    // place on a hot reload, and `matched.route` points into `routes_`, so
    // neither may be read once the request leaves the current turn. `client`
    // keeps the client Socket alive even if its connection entry is torn down
    // (idle timeout) while a backend is busy.
    struct ExchangeState {
        std::shared_ptr<Socket> client;
        HttpRequest req;
        std::vector<std::unique_ptr<ChildExchange>> children;
        // Composite aggregate mode. When a total deadline is set it is a hard
        // wall-clock bound checked by the loop, independent of each child's idle
        // deadline (which UpstreamExchange refreshes on I/O progress).
        bool composite = false;
        std::string route_name;          // aggregate "route" field, snapshotted
        std::string composite_name;      // composite_target.name: aggregate "aggregate" field, snapshotted
        int total_timeout_ms = 0;
        bool has_total_deadline = false;
        std::chrono::steady_clock::time_point total_deadline;
        std::chrono::steady_clock::time_point started;
        std::string client_ip;
    };

    Epoll& epoll_;
    // Per-connection read buffer (for assembling headers)
    std::unordered_map<Socket*, std::string> read_bufs_;
    // Per-connection pending response data. std::string (not vector<char>) lets a
    // fully-built header buffer be moved into the queue instead of copied, and
    // still exposes data()/size()/erase() for the send path.
    std::unordered_map<Socket*, std::deque<std::string>> send_queues_;
    // Whether a complete request has been parsed, per connection
    std::unordered_map<Socket*, bool> request_ready_;
    // Parser instances (one per connection)
    std::unordered_map<Socket*, HttpParser> parsers_;
    // Parsed requests (valid only when request_ready_ is true)
    std::unordered_map<Socket*, HttpRequest> requests_;
    // Whether the connection is kept alive
    std::unordered_map<Socket*, bool> keep_alive_;
    std::unordered_map<Socket*, int> file_fds_; // fd of the file being sent
    std::unordered_map<Socket*, off_t> file_offsets_;    // Current send offset
    std::unordered_map<Socket*, off_t> file_sizes_;      // Total file size
    std::unordered_map<Socket*, std::vector<char>> ssl_file_buffers_; // Stable while an SSL_write is pending
    std::unordered_map<Socket*, SSLWant> pending_ssl_writes_; // Event direction needed to resume SSL_write
    std::unordered_map<Socket*, int> resp_status_;      // Status code
    std::unordered_map<Socket*, size_t> resp_size_;     // Total bytes to send
    std::string www_root_;      // Document root
    FileCache cache_;   // File cache

    void send_response(std::shared_ptr<Socket> sock, const HttpRequest& req, const ResolvedRoute& matched);
    // Frame + enqueue a fully built response and arm EPOLLOUT. Shared by the
    // synchronous path and by the upstream completion path.
    void finalize_response(Socket* sock, const HttpRequest& req, HttpResponse& resp, bool already_compressed = false);
    // Hand an upstream route to the event loop as a non-blocking UpstreamExchange:
    // build the snapshotted params, pick a backend, register the upstream fd and
    // return. The backend round trip consumes no thread while it waits.
    void start_upstream_exchange(std::shared_ptr<Socket> sock, const HttpRequest& req, const ResolvedRoute& matched);
    // Hand a composite route to the event loop: fan out every dependency as a
    // non-blocking upstream exchange owned by this connection, snapshotting the
    // dependency list and the optional overall deadline. Returns immediately; the
    // aggregate is framed when all children finish or the overall deadline fires.
    void start_composite_exchange(std::shared_ptr<Socket> sock, const HttpRequest& req, const ResolvedRoute& matched, const std::string& client_ip);
    // Drive one step of child `idx` owned by `client` and apply the result:
    // (re)register its upstream fd, or, on Done, unregister it and finish it.
    void drive_child(Socket* client, ExchangeState* st, size_t idx, uint32_t events);
    // Drive one step of the single child (index 0) of a single-upstream exchange.
    void drive_exchange(Socket* client, uint32_t events);
    // Called when child `idx` finishes: record the circuit outcome, then either
    // frame the single-upstream response (retrying first when the route allows
    // it) or fold the result into the composite item.
    void finish_child(Socket* client, ExchangeState* st, size_t idx);
    // Composite only: abandon an unfinished child because the overall deadline
    // fired, synthesizing a timed-out item so the aggregate can still complete.
    void force_finish_child(ExchangeState* st, size_t idx);
    // Aggregate the completed composite children and frame the response; called
    // once every child has finished.
    void finalize_composite(Socket* client, ExchangeState* st);
    // Drop an in-flight upstream exchange for a connection that is going away:
    // unregister its upstream fd and free the state without sending anything.
    void abort_upstream_for_client(Socket* client);
    // Re-arm the client fd for reading, unless an upstream round trip is in
    // flight on this connection (EPOLLONESHOT is intentionally left disarmed
    // until the response is delivered, so a pipelined second request cannot run
    // concurrently with the first).
    void rearm_reads(Socket* sock);
    void dispatch_route(const HttpRequest& req, const ResolvedRoute& matched, HttpResponse& resp);
    ResolvedRoute resolve_route(const HttpRequest& req) const;
    void register_default_routes();
    void register_configured_routes();
    void wait_for_ssl_write(Socket* sock, SSLWant want);
    HttpResponse make_error_response(int code, const std::string& status, const std::string& message) const;

    void send_error_response(Socket* sock, int code, const std::string& message,
                             const std::string& allow_methods = "");

    // Helper: serialize response headers (without body)
    std::string headers_to_string(const HttpResponse& resp);

    std::vector<RegisteredRoute> routes_;

    std::unordered_map<Socket*, std::string> client_ip_map_;
    std::unordered_map<Socket*, std::chrono::steady_clock::time_point> request_start_time_;
    std::unordered_map<Socket*, HttpRequest> last_requests_; // Last request, kept for logging
    FdCache fd_cache_;   // File descriptor cache
    Config config_;                      // Runtime config snapshot, updatable via reload

    UpstreamManager& upstream_manager_;  // Reference used to select backends

    // ── In-flight upstream round trips (event-loop owned) ──
    // One entry per client connection with backend call(s) outstanding, plus a
    // reverse index from an upstream fd to (owning client, child index) so the
    // worker can dispatch that fd's event straight to the right child exchange.
    // Both live only on the event-loop thread.
    std::unordered_map<Socket*, std::unique_ptr<ExchangeState>> client_upstream_;
    std::unordered_map<int, std::pair<Socket*, size_t>> upstream_fd_owner_;

    std::shared_ptr<RateLimiterManager> rate_limiter_manager_;   // Rate limiter shared across workers
    ApiKeyManager& api_key_manager_;                             // API key authentication

    // Whether the request requires auth (public routes like static files and /metrics need none)
    bool requires_auth(const HttpRequest& req) const;
    bool should_rate_limit(const HttpRequest& req, const GatewayRoute& route, const std::string& client_ip, const ApiKeyConfig* api_key_cfg) const;
    // ── Public API ──
public:
    explicit HttpHandler(Epoll& epoll, const Config& config,UpstreamManager& upstream_manager,ApiKeyManager& api_key_manager, std::shared_ptr<RateLimiterManager> rate_limiter_manager);
    ~HttpHandler();
    void reload_config(const Config& config);
    void on_connect(Socket* sock);                          // Initialize
    void handle_read(std::shared_ptr<Socket> sock,const std::string& client_ip);         // Handle read events
    void handle_write(std::shared_ptr<Socket> sock);
    bool has_pending_ssl_write(Socket* sock) const;
    void cleanup(std::shared_ptr<Socket> sock);
    // True when the connection still has work to finish: a partially received
    // request, a parsed request awaiting dispatch, response data queued for
    // sending, or an upstream round trip still in flight. Used by the
    // graceful-shutdown drain phase.
    bool has_inflight_work(Socket* sock) const;
    // True when `fd` is an in-flight upstream socket owned by an exchange.
    bool is_upstream_fd(int fd) const;
    // Drives the exchange owning `fd` for the given epoll event mask. Must be
    // called on the event-loop thread.
    void on_upstream_event(int fd, uint32_t events);
    // epoll_wait budget: the nearest upstream idle deadline (capped at 1s) so a
    // backend timeout is noticed on time even when the loop is otherwise busy.
    int poll_timeout_ms() const;
    // Fails exchanges whose idle deadline elapsed. Called by the worker after
    // every epoll_wait iteration, including the graceful-shutdown drain.
    void check_upstream_timeouts();
    void addRoute(const std::string& method, const std::string& pattern, RouteHandler handler);    // Register a route: method is "GET", "POST", etc.; path like "/api/hello"
    void addRoute(const GatewayRoute& route, RouteHandler handler);
    std::string extract_api_key(const HttpRequest& req);
};