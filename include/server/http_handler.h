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
#include "fd_cache.h"
#include "config.h"
#include "upstream_manager.h"
#include "rate_limiter.h"
#include "rate_limiter_manager.h"
#include "api_key_manager.h"

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

    Epoll& epoll_;
    // Per-connection read buffer (for assembling headers)
    std::unordered_map<Socket*, std::string> read_bufs_;
    // Per-connection pending response data; deque<vector<char>> optimizes front removal
    std::unordered_map<Socket*, std::deque<std::vector<char>>> send_queues_;
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
    std::unordered_map<Socket*, int> resp_status_;      // Status code
    std::unordered_map<Socket*, size_t> resp_size_;     // Total bytes to send
    std::string www_root_;      // Document root
    FileCache cache_;   // File cache

    void send_response(Socket* sock, const HttpRequest& req, const ResolvedRoute& matched);
    void dispatch_route(const HttpRequest& req, const ResolvedRoute& matched, HttpResponse& resp) const;
    ResolvedRoute resolve_route(const HttpRequest& req) const;
    void register_default_routes();
    void register_configured_routes();
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

    std::shared_ptr<RateLimiterManager> rate_limiter_manager_;   // Rate limiter shared across workers
    ApiKeyManager& api_key_manager_;                             // API key authentication

    // Whether the request requires auth (public routes like static files and /metrics need none)
    bool requires_auth(const HttpRequest& req) const;
    bool should_rate_limit(const HttpRequest& req, const GatewayRoute& route, const std::string& client_ip, const ApiKeyConfig* api_key_cfg) const;
public:
    explicit HttpHandler(Epoll& epoll, const Config& config,UpstreamManager& upstream_manager,ApiKeyManager& api_key_manager, std::shared_ptr<RateLimiterManager> rate_limiter_manager);
    void reload_config(const Config& config);
    void on_connect(Socket* sock);                          // Initialize
    void handle_read(std::shared_ptr<Socket> sock,const std::string& client_ip);         // Handle read events
    void handle_write(std::shared_ptr<Socket> sock);
    void close_connection(std::shared_ptr<Socket> sock);
    void cleanup(std::shared_ptr<Socket> sock);
    void process_request(Socket* sock_ptr);
    void addRoute(const std::string& method, const std::string& pattern, RouteHandler handler);    // Register a route: method is "GET", "POST", etc.; path like "/api/hello"
    void addRoute(const GatewayRoute& route, RouteHandler handler);
    std::string extract_api_key(const HttpRequest& req);
};