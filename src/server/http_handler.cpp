#include "http_handler.h"
#include <sys/epoll.h>
#include <sys/stat.h>       // needed for struct stat
#include "content_type.h"
#include <sys/sendfile.h>       // needed for sendfile
#include <fcntl.h>             // needed for open
#include <unistd.h>            // needed for close
#include <sstream>             // needed for ostringstream
#include <algorithm>           // needed for std::transform
#include <cctype>              // needed for std::tolower
#include <stdexcept>           // needed for std::runtime_error
#include "mylogger.h"
#include <iomanip>      // needed for std::hex
#include <nlohmann/json.hpp>
#include "route_utils.h"
#include "gzip_utils.h"
#include <string>
#include "metrics.h"
#include "http_client.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include "upstream_manager.h"
#include <atomic>
#include <cstdint>
using json = nlohmann::json;

namespace {

std::atomic<uint64_t> g_trace_counter{0};

std::string generate_trace_id() {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const uint64_t seq = ++g_trace_counter;
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << (now_ns & 0xffffffffULL)
        << std::setw(8) << std::setfill('0') << (seq & 0xffffffffULL);
    return oss.str();
}

std::string classify_failure_reason(int status_code, const BackendError* error = nullptr) {
    if (status_code >= 200 && status_code < 400) return "none";
    if (status_code == 401) return "auth_failed";
    if (status_code == 403) return "forbidden";
    if (status_code == 404) return "route_not_found";
    if (status_code == 405) return "method_not_allowed";
    if (status_code == 413) return "payload_too_large";
    if (status_code == 429) return "rate_limited";
    if (status_code == 503) return "upstream_circuit_open";
    if (status_code == 502) return error == nullptr ? "upstream_unavailable" : "upstream_unavailable";
    if (status_code == 504) return "upstream_timeout";
    if (status_code == 500) return "internal_error";
    if (error != nullptr) {
        switch (*error) {
            case BackendError::ConnectFailed: return "connect_failed";
            case BackendError::ConnectTimeout: return "connect_timeout";
            case BackendError::WriteFailed: return "write_failed";
            case BackendError::WriteTimeout: return "write_timeout";
            case BackendError::ReadFailed: return "read_failed";
            case BackendError::ReadTimeout: return "read_timeout";
            case BackendError::InvalidResponse: return "invalid_upstream_response";
            default: return "unknown_error";
        }
    }
    return "unknown_error";
}

std::string redact_api_key(const std::string& key) {
    if (key.empty()) {
        return "anonymous";
    }
    if (key.size() <= 8) {
        return "[redacted]";
    }
    return key.substr(0, 4) + "..." + key.substr(key.size() - 4);
}

void emit_audit_log(const HttpRequest& req, int status_code, const std::string& failure_reason, const std::string& route_name, const std::string& host, const std::string& tenant) {
    const std::string trace_id = req.trace_id.empty() ? "unknown" : req.trace_id;
    const std::string key = req.headers.count("x-api-key") ? req.headers.at("x-api-key") : "";
    const std::string api_key = redact_api_key(key);
    Logger::get()->info(
        "AUDIT trace_id={} method={} path={} host={} tenant={} route={} status={} failure={} api_key={} user_agent={}",
        trace_id,
        req.method,
        req.path,
        host,
        tenant,
        route_name,
        status_code,
        failure_reason,
        api_key,
        req.headers.count("user-agent") ? req.headers.at("user-agent") : "-");
}

} // anonymous namespace

static std::string to_hex(size_t n) {
    std::ostringstream oss;
    oss << std::hex << n;
    return oss.str();
}


HttpHandler::HttpHandler(Epoll& epoll, const Config& config, UpstreamManager& upstream_manager, ApiKeyManager& api_key_manager, std::shared_ptr<RateLimiterManager> rate_limiter_manager) : epoll_(epoll), www_root_(config.www_root), config_(config), 
cache_(config.cache_max_entries, config.cache_max_file_size_mb), 
upstream_manager_(upstream_manager), api_key_manager_(api_key_manager), rate_limiter_manager_(std::move(rate_limiter_manager)) {
    register_default_routes();
    register_configured_routes();
}

void HttpHandler::reload_config(const Config& config) {
    config_ = config;
    www_root_ = config.www_root;
    upstream_manager_.reload(config.upstream_config);
    api_key_manager_.reload(config.api_keys);
    rate_limiter_manager_->update_default_config(config.rate_limit_config);
    routes_.clear();
    register_default_routes();
    register_configured_routes();
    Logger::get()->info("Runtime configuration reloaded: {} routes, {} upstreams",
                        config_.upstream_config.routes.size(),
                        config_.upstream_config.upstreams.size());
}

void HttpHandler::register_default_routes() {
    addRoute("GET", "/metrics", [](const HttpRequest& req, HttpResponse& resp, const RouteParams&) {
        std::string body = Metrics::instance().to_string();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "text/plain; version=0.0.4";
        resp.body = body;
        resp.headers["Content-Length"] = std::to_string(body.size());
    });

    addRoute("GET", "/api/hello", [](const HttpRequest& req, HttpResponse& resp,const RouteParams& params) {
        nlohmann::json j;
        j["message"] = "Hello, World!";
        std::string body = j.dump();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = body;
        resp.headers["Content-Length"] = std::to_string(body.size());
    });

    addRoute("POST", "/api/echo", [](const HttpRequest& req, HttpResponse& resp,const RouteParams& params) {
        try {
            auto j = nlohmann::json::parse(req.body);
            nlohmann::json resp_json;
            resp_json["echo"] = j;
            std::string body = resp_json.dump();
            resp.status_code = 200;
            resp.status_message = "OK";
            resp.headers["Content-Type"] = "application/json";
            resp.body = body;
            resp.headers["Content-Length"] = std::to_string(body.size());
        } catch (...) {
            resp.status_code = 400;
            resp.status_message = "Bad Request";
            resp.body = "{\"error\":\"Invalid JSON\"}";
            resp.headers["Content-Type"] = "application/json";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
        }
    });

    addRoute("PUT", "/api/echo", [](const HttpRequest& req, HttpResponse& resp,const RouteParams& params) {
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "text/plain";
        resp.body = "PUT received: " + req.body;
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
    });

    addRoute("DELETE", "/api/resource", [](const HttpRequest& req, HttpResponse& resp,const RouteParams& params) {
        json j;
        j["status"] = "deleted";
        j["message"] = "Resource deleted successfully";
        resp.body = j.dump();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
    });

    addRoute("GET", "/users/{id}", [](const HttpRequest& req, HttpResponse& resp, const RouteParams& params) {
        std::string user_id = params.at("id");
        json j;
        j["id"] = user_id;
        j["name"] = "User_" + user_id;  // mock data
        resp.body = j.dump();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
    });

    addRoute("GET", "/chunked", [](const HttpRequest& req, HttpResponse& resp, const RouteParams&) {
        std::string payload = "This is a chunked response.\n";
        payload += "Each line could be generated separately.\n";
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "text/plain";
        resp.chunked = true;      // Key: set the chunked flag
        resp.body = payload;
    });
}

void HttpHandler::register_configured_routes() {
    for (const auto& route : config_.upstream_config.routes) {
        GatewayRoute route_policy = route;
        route_policy.target_type = route.target_type.empty() ? "upstream" : route.target_type;
        addRoute(route_policy, {});
    }
}

void HttpHandler::on_connect(Socket* sock){
    read_bufs_[sock];          // Create an empty read buffer
    send_queues_[sock];        // Create an empty send queue
    request_ready_[sock] = false;
    parsers_[sock] = HttpParser();
    keep_alive_[sock] = true;  // keep-alive by default
}

void HttpHandler::handle_read(std::shared_ptr<Socket> sock,const std::string& client_ip){
    Socket* sock_ptr = sock.get();
    client_ip_map_[sock_ptr] = client_ip;
    request_start_time_[sock_ptr] = std::chrono::steady_clock::now();
    int fd = sock->getFd();
    try {
        char buf[4096];
        while (true) {
            bool is_ssl = sock_ptr->get_is_ssl_();
            int n = is_ssl ? sock_ptr->sslRead(buf, sizeof(buf)) : sock_ptr->recv(buf, sizeof(buf), 0);
            if (n > 0) {
                auto& read_buf = read_bufs_[sock_ptr];
                read_buf.append(buf, n);

                // Keep parsing the buffer until no complete request remains
                while (true) {
                    size_t consumed = 0;
                    if (parsers_[sock_ptr].parse(read_buf.data(), read_buf.size(),requests_[sock_ptr], consumed)) {
                        // Parsed a complete request; record the total request count
                        Metrics::instance().record_total_request();
                        // Remove the consumed data from the buffer
                        read_buf.erase(0, consumed);

                        // Get a reference to the parsed request
                        auto& req = requests_[sock_ptr];

                        const auto host_it = req.headers.find("host");
                        const auto tenant_it = req.headers.find("x-tenant-id");
                        if (host_it != req.headers.end()) req.host = host_it->second;
                        if (tenant_it != req.headers.end()) req.tenant = tenant_it->second;
                        if (req.trace_id.empty()) {
                            req.trace_id = req.headers.count("x-trace-id") ? req.headers["x-trace-id"] : generate_trace_id();
                        }
                        req.headers["x-trace-id"] = req.trace_id;

                        auto resolved = resolve_route(req);
                        if (!resolved.route) {
                            if (resolved.path_matched) {
                                Logger::get()->debug("Method {} is not allowed for {} on fd {}",
                                                     req.method, req.path, fd);
                                last_requests_[sock_ptr] = req;
                                send_error_response(sock_ptr, 405, "Method Not Allowed", resolved.allow_methods);
                                parsers_[sock_ptr].reset();
                                continue;
                            }
                            // No route matched: still hand off to send_response, letting the static-file fallback decide the outcome.
                            Logger::get()->debug("No matching route for {} {} on fd {}", req.method, req.path, fd);
                            last_requests_[sock_ptr] = req;
                            send_response(sock_ptr, req, resolved);
                            parsers_[sock_ptr].reset();
                            continue;
                        }

                        const GatewayRoute& route_policy = resolved.route->route;
                        const std::string route_name = route_policy.name.empty() ? route_policy.path : route_policy.name;
                        req.headers["x-route-name"] = route_name;

                        // ─── Auth check: the route policy decides whether auth is required ───
                        const ApiKeyConfig* api_key_cfg = nullptr;
                        if (route_policy.auth_required && !route_policy.allow_anonymous) {
                            std::string key = extract_api_key(req);
                            auto host_it = req.headers.find("host");
                            auto tenant_it = req.headers.find("x-tenant-id");
                            const std::string host = host_it == req.headers.end() ? "" : host_it->second;
                            const std::string tenant = tenant_it == req.headers.end() ? "" : tenant_it->second;
                            if (key.empty() || !api_key_manager_.authorize(key, route_policy, host, tenant)) {
                                Logger::get()->debug("Authentication failed for {} {} on fd {}", req.method, req.path, fd);
                                last_requests_[sock_ptr] = req;
                                send_error_response(sock_ptr, 401, "Unauthorized");
                                parsers_[sock_ptr].reset();
                                continue;
                            }
                            api_key_cfg = api_key_manager_.get(key);
                        }

                        // ─── Rate limit check: policy chosen per route_policy ───
                        if (should_rate_limit(req, route_policy, client_ip, api_key_cfg)) {
                            Logger::get()->debug("Rate limit exceeded for {} {} on fd {}", req.method, req.path, fd);
                            last_requests_[sock_ptr] = req;
                            send_error_response(sock_ptr, 429, "Too Many Requests");
                            parsers_[sock_ptr].reset();
                            continue;
                        }

                        // ─── Method validation (final gate before route handling) ───
                        if (req.method != "GET" && req.method != "HEAD" && req.method != "POST" && req.method != "PUT" && req.method != "DELETE") {
                            Logger::get()->debug("HTTP method not allowed: {} on fd {}", req.method, fd);
                            last_requests_[sock_ptr] = req;
                            send_error_response(sock_ptr, 405, "Method Not Allowed");
                            parsers_[sock_ptr].reset();
                            continue;
                        }
                        
                        // Record user-agent (keys are normalized to lowercase)
                        auto ua_it = req.headers.find("user-agent");
                        std::string user_agent = (ua_it != req.headers.end()) ? ua_it->second : "-";
                        // The CLF access log is written after the response completes; keep this entry-level diagnostic but avoid duplicate info logs in production.
                        Logger::get()->debug("Request: {} {} {} - UA: {}", req.method, req.path, req.version, user_agent);

                        // Check the connection header to decide keep-alive (keys are normalized to lowercase)
                        auto it = req.headers.find("connection");
                        if (it != req.headers.end()) {
                            std::string conn = it->second;
                            std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
                            keep_alive_[sock_ptr] = (conn != "close");
                        } else {
                            keep_alive_[sock_ptr] = true; // HTTP/1.1 defaults to keep-alive
                        }

                        // Prepare the response (using the request parsed above)
                        send_response(sock_ptr, req, resolved);

                        // Reset the parser for the next request
                        parsers_[sock_ptr].reset();

                        // ★ Key: break out of the inner loop; handle the next request after sending completes
                        break;
                    }
                    // ─── Check whether the body exceeds the size limit ───
                    if (parsers_[sock_ptr].is_body_too_large()) {
                        Logger::get()->warn("Request body too large (>{}) on fd {}",
                            HttpParser::MAX_BODY_SIZE, fd);
                        last_requests_[sock_ptr] = requests_[sock_ptr];
                        send_error_response(sock_ptr, 413, "Payload Too Large");
                        // Remove partially consumed data
                        if (consumed > 0) read_buf.erase(0, consumed);
                        parsers_[sock_ptr].reset();
                        requests_[sock_ptr].clear();
                        continue;  // Keep checking the buffer for further requests
                    }
                    else {
                        Logger::get()->trace("Parser waiting for more data, buffer size: {}", read_buf.size());
                        break; // No complete request yet, wait for more data
                    }
                }
            } else if (n == 0) {
                cleanup(sock);
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                else { throw_system_error("recv"); }
            }
        }
        // Re-arm read events (if the connection is still alive)
        if (!send_queues_[sock_ptr].empty()) {
            epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
        } else {
            epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
        }
    } catch (const std::exception& e) {
        Logger::get()->error("Exception in handle_read: {}", e.what());
        cleanup(sock);
    } catch (...) {
        Logger::get()->error("Unknown exception in handle_read");
        cleanup(sock);
    }
}

void HttpHandler::process_request(Socket* sock_ptr){
    HttpRequest& req = requests_[sock_ptr];
    // Check the connection header (keys are normalized to lowercase)
    auto it = req.headers.find("connection");
    if (it != req.headers.end()) {
        std::string conn = it->second;
        std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
        keep_alive_[sock_ptr] = (conn == "keep-alive");
    } else {
        keep_alive_[sock_ptr] = true; // HTTP/1.1 defaults to keep-alive
    }
    auto matched = resolve_route(req);
    send_response(sock_ptr, req, matched); // Prepare the response
    request_ready_[sock_ptr] = false; // Allow the next request to be parsed
    parsers_[sock_ptr].reset();       // Reset the parser state
}

HttpResponse HttpHandler::make_error_response(int code, const std::string& status, const std::string& message) const {
    HttpResponse resp;
    resp.status_code = code;
    resp.status_message = status;
    resp.body = "<h1>" + std::to_string(code) + " " + message + "</h1>";
    resp.headers["Content-Type"] = "text/html";
    resp.headers["Content-Length"] = std::to_string(resp.body.size());
    resp.headers["Connection"] = "close";
    return resp;
}

HttpHandler::ResolvedRoute HttpHandler::resolve_route(const HttpRequest& req) const {
    ResolvedRoute matched;
    for (const auto& registered : routes_) {
        RouteParams params;
        const auto host_it = req.headers.find("host");
        const auto tenant_it = req.headers.find("x-tenant-id");
        const std::string host = host_it == req.headers.end() ? "" : host_it->second;
        const std::string tenant = tenant_it == req.headers.end() ? "" : tenant_it->second;
        if (routeMatchesPath(registered.route, host, tenant, req.path, params)) {
            matched.path_matched = true;
            const std::string method = registered.route.method.empty() ? "*" : registered.route.method;
            if (!matched.allow_methods.empty()) {
                matched.allow_methods += ", ";
            }
            matched.allow_methods += method;
        }
        if (routeMatchesRequest(registered.route, req.method, host, tenant, req.path, params)) {
            matched.route = &registered;
            matched.params = std::move(params);
            return matched;
        }
    }
    return matched;
}

void HttpHandler::dispatch_route(const HttpRequest& req, const ResolvedRoute& matched, HttpResponse& resp) const {
    if (!matched.route) {
        resp.status_code = 404;
        resp.status_message = "Not Found";
        resp.body = "<h1>404 Not Found</h1>";
        resp.headers["Content-Type"] = "text/html";
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
        return;
    }

    if (matched.route->route.target_type == "local" && matched.route->handler) {
        matched.route->handler(req, resp, matched.params);
        return;
    }

    if (matched.route->route.target_type == "upstream") {
        const auto& route = matched.route->route;
        const std::string& upstream_name = route.upstream_target.name;
        auto it = config_.upstream_config.upstreams.find(upstream_name);
        if (it == config_.upstream_config.upstreams.end() || it->second.servers.empty()) {
            resp.status_code = 502;
            resp.status_message = "Bad Gateway";
            resp.body = "Bad Gateway: no upstream server";
            resp.headers["Content-Type"] = "text/plain";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
            return;
        }

        const auto& server = upstream_manager_.pick_server(upstream_name);
        // The circuit-breaker check happens before connecting to the backend; no new backend connection is made while the circuit is open.
        if (!upstream_manager_.allow_request(upstream_name, server,route.upstream_target.circuit_recovery_timeout_ms)) {
            resp = make_error_response(503, "Service Unavailable", "Upstream circuit is open");
            return;
        }

        BackendResponse be;
        be.status_code = 502;
        be.body = "Bad Gateway";
        be.error = BackendError::ConnectFailed;
        int attempt_count = 0;
        while (true) {
            be = forward_request(server.host, server.port, req.method, req.path,
                                 req.headers, req.body,
                                 route.upstream_target.timeout_ms);
            if (be.error == BackendError::None || attempt_count >= route.upstream_target.max_retries ||
                !should_retry_backend_request(req.method, be.error, attempt_count)) {
                break;
            }
            ++attempt_count;
        }
        if (be.error != BackendError::None) {
            upstream_manager_.record_failure(upstream_name, server,route.upstream_target.circuit_failure_threshold,route.upstream_target.circuit_recovery_timeout_ms);
            Metrics::instance().record_upstream_error(be.error);
            const bool timed_out = be.error == BackendError::ConnectTimeout ||
                                   be.error == BackendError::WriteTimeout ||
                                   be.error == BackendError::ReadTimeout;
            resp = make_error_response(
                timed_out ? 504 : 502,
                timed_out ? "Gateway Timeout" : "Bad Gateway",
                timed_out ? "The upstream service timed out"
                          : "The upstream service returned an invalid response or could not be reached");
            return;
        }
        upstream_manager_.record_success(upstream_name, server);
        resp.status_code = be.status_code;
        resp.status_message = be.status_code >= 200 && be.status_code < 300 ? "OK" : "Upstream Response";
        resp.body = be.body;
        for (const auto& [k, v] : be.headers) {
            resp.headers[k] = v;
        }
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
        return;
    }

    if (matched.route->route.target_type == "static") {
        std::string static_root = matched.route->route.static_root.empty() ? config_.www_root : matched.route->route.static_root;
        std::string resolved;
        const std::string request_path = req.path.empty() ? "/" : req.path;
        if (!is_safe_static_path("/", request_path, static_root, resolved)) {
            resp.status_code = 403;
            resp.status_message = "Forbidden";
            resp.body = "<h1>403 Forbidden</h1>";
            resp.headers["Content-Type"] = "text/html";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
            return;
        }

        std::ifstream file(resolved, std::ios::binary);
        if (!file.is_open()) {
            resp.status_code = 404;
            resp.status_message = "Not Found";
            resp.body = "<h1>404 Not Found</h1>";
            resp.headers["Content-Type"] = "text/html";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
            return;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.body = buffer.str();
        resp.headers["Content-Type"] = get_content_type(resolved);
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
        return;
    }

    resp.status_code = 404;
    resp.status_message = "Not Found";
    resp.body = "<h1>404 Not Found</h1>";
    resp.headers["Content-Type"] = "text/html";
    resp.headers["Content-Length"] = std::to_string(resp.body.size());
}

void HttpHandler::send_response(Socket* sock, const HttpRequest& req, const ResolvedRoute& matched){
    last_requests_[sock] = req;
    HttpResponse resp;
    resp.headers["X-Trace-Id"] = req.trace_id.empty() ? generate_trace_id() : req.trace_id;
    std::string path = req.path;

    // Default index page
    if (path.empty() || path == "/") path = "/index.html"; // Default index page

    bool path_handled = false;

    if (matched.route) {
        dispatch_route(req, matched, resp);
        resp.headers["X-Trace-Id"] = req.trace_id.empty() ? generate_trace_id() : req.trace_id;
        path_handled = true;
    }

    bool already_compressed = false;
    
    if (!path_handled) {
        // When no route matched, the static fallback only allows GET/HEAD, preventing methods like POST from reading static files.
        if (req.method != "GET" && req.method != "HEAD") {
            resp.status_code = 405;
            resp.status_message = "Method Not Allowed";
            resp.body = "<h1>405 Method Not Allowed</h1>";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
            resp.headers["Content-Type"] = "text/html";
            resp.chunked = false;
        }
        // Guard against path traversal attacks; simple approach: reject ".."
        else if (path.find("..") != std::string::npos) {
            // Return 403 Forbidden
            resp.status_code = 403;
            resp.status_message = "Forbidden";
            resp.body = "<h1>403 Forbidden</h1>";       //<h1>: HTML heading tag, rendered in large font
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
            resp.headers["Content-Type"] = "text/html";
            resp.chunked = false;
        }
        else{
            Logger::get()->debug("Attempting to serve file: {}", path);

            // Large files: sendfile zero-copy + fd cache optimization
            int file_fd = -1;

            // Fast path: return directly within the TTL, zero syscalls
            struct stat st;
            off_t cached_size = 0;
            time_t cached_mtime = 0;
            time_t now = time(nullptr);
            file_fd = fd_cache_.try_get(path, now, &cached_size, &cached_mtime);
            if (file_fd != -1) {
                // Cache hit! Use directly, no stat/open/fstat
                Metrics::instance().record_fd_cache_hit();  // Record a cache hit
                st.st_size = cached_size;
                st.st_mtime = cached_mtime;
                Logger::get()->debug("FdCache fast hit: {}", path);
            }
            else {
                // TTL expired or not cached → need stat() to verify
                std::string file_path = www_root_ + path;
                if (::stat(file_path.c_str(), &st) != 0) {
                    resp.status_code = 404;
                    resp.status_message = "Not Found";
                    resp.body = "<h1>404 Not Found</h1>";
                    resp.headers["Content-Length"] = std::to_string(resp.body.size());
                    resp.headers["Content-Type"] = "text/html";
                    resp.chunked = false;
                    goto after_file;
                }

                // Try to validate an existing cache entry
                file_fd = fd_cache_.validate(path, st.st_mtime);
                if (file_fd != -1) {
                    Metrics::instance().record_fd_cache_hit();  // Record a cache hit
                    Logger::get()->debug("FdCache validated: {}", path);
                }
                else {
                    // Not in the cache, perform open
                    Metrics::instance().record_fd_cache_miss();
                    file_fd = open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
                    if (file_fd >= 0) {
                        fd_cache_.put(path, file_fd, st.st_mtime, st.st_size);
                        Logger::get()->debug("FdCache miss, stored: {}", path);
                    }
                    else {
                        resp.status_code = 500;
                        resp.status_message = "Internal Server Error";
                        resp.body = "<h1>500 Internal Server Error</h1>";
                        resp.headers["Content-Length"] = std::to_string(resp.body.size());
                        resp.headers["Content-Type"] = "text/html";
                        resp.chunked = false;
                        goto after_file;
                    }
                }
            }
            
            if (file_fd >= 0) {
                // st was already filled by stat(), no need for fstat
                constexpr size_t MAX_INLINE = 1024 * 1024; // 1MB

                // Check whether the client supports gzip
                bool client_wants_gzip = false;
                auto it = req.headers.find("accept-encoding");
                if (it != req.headers.end() && it->second.find("gzip") != std::string::npos) {
                    client_wants_gzip = true;
                }

                    if (client_wants_gzip && st.st_size <= MAX_INLINE) {
                        // Try to get the compressed cache entry
                        std::string gzip_key = path + "#gzip";
                        const std::string* compressed_cached = cache_.get(gzip_key, st.st_mtime);
                        if (compressed_cached) {
                            // Compressed cache hit
                            Metrics::instance().record_gzip_cache_hit();   // ★ Use the gzip-specific counter
                            resp.body = *compressed_cached;
                            resp.headers["Content-Encoding"] = "gzip";
                            already_compressed = true;
                            Logger::get()->debug("Cache hit (gzip): {}", path);
                        } else {
                            // Cache miss: fetch the raw content, compress it, and cache the compressed result
                            const std::string* raw = cache_.get(path, st.st_mtime);
                            if (!raw) {
                                // Raw content not cached either: read the file and store it in the raw cache
                                Metrics::instance().record_cache_miss();
                                std::string file_content(st.st_size, '\0');
                                ssize_t n = ::read(file_fd, &file_content[0], st.st_size);
                                if (n == static_cast<ssize_t>(st.st_size)) {
                                    cache_.put(path, file_content, st.st_size, st.st_mtime);
                                    resp.body = std::move(file_content);
                                    raw = &resp.body;  // ★ Fix: point to resp.body for later compression
                                }
                                else{
                                    //close(file_fd);
                                    resp.status_code = 500;
                                    resp.status_message = "Internal Server Error";
                                    resp.body = "<h1>500 Internal Server Error</h1>";
                                    resp.headers["Content-Type"] = "text/html";
                                    resp.headers["Content-Length"] = std::to_string(resp.body.size());

                                    resp.chunked = false;
                                    goto after_file;  // Jump out of file handling
                                }
                            }
                            std::string compressed;
                            if (gzip_compress(*raw, compressed)) {
                                cache_.put(gzip_key, compressed, compressed.size(), st.st_mtime);
                                resp.body = std::move(compressed);
                                resp.headers["Content-Encoding"] = "gzip";
                                already_compressed = true;
                                Metrics::instance().record_gzip_cache_miss();  // ★ Use the gzip-specific counter
                            } else if (raw != &resp.body) {
                                // Compression failed, fall back to the raw content (avoid self-copy)
                                resp.body = *raw;
                            }
                            Logger::get()->debug("Gzip cache miss, compressed: {}", already_compressed);
                        }
                        // Either way, the small file is already in memory, so the file can be closed
                        // close(file_fd);
                        // file_fd = -1;  // mark as closed
                        resp.status_code = 200;
                        resp.status_message = "OK";
                        resp.headers["Content-Type"] = get_content_type(path);
                        resp.headers["Content-Length"] = std::to_string(resp.body.size());
                    }else{
                        // ---------- No gzip support or large file: original path ----------
                        if (st.st_size <= MAX_INLINE) {
                            // Small file without compression: direct in-memory cache (original cache logic)
                            const std::string* cached = cache_.get(path, st.st_mtime);
                            if (cached) {
                                resp.body = *cached;
                                Logger::get()->debug("Cache hit (raw): {}", path);
                                Metrics::instance().record_cache_hit();
                            } else {
                                std::string file_content(st.st_size, '\0');
                                ssize_t n = ::read(file_fd, &file_content[0], st.st_size);
                                if (n == static_cast<ssize_t>(st.st_size)) {
                                    cache_.put(path, file_content, st.st_size, st.st_mtime);
                                    resp.body = std::move(file_content);
                                    Metrics::instance().record_cache_miss();
                                    Logger::get()->debug("Cache miss, stored (raw): {}", path);
                                } else {
                                    // fd is cached; FdCache owns its lifetime, do not close it
                                    resp.status_code = 500;
                                    resp.status_message = "Internal Server Error";
                                    resp.body = "<h1>500 Internal Server Error</h1>";
                                    resp.headers["Content-Type"] = "text/html";
                                    resp.headers["Content-Length"] = std::to_string(resp.body.size());
                                    resp.chunked = false;
                                    goto after_file;
                                }
                            }
                            // fd is cached; FdCache owns its lifetime, do not close it
                            resp.status_code = 200;
                            resp.status_message = "OK";
                            resp.headers["Content-Type"] = get_content_type(path);
                            resp.headers["Content-Length"] = std::to_string(resp.body.size());
                        } else {
                            // Large file: sendfile zero-copy
                            resp.status_code = 200;
                            resp.status_message = "OK";
                            resp.headers["Content-Type"] = get_content_type(path);
                            resp.headers["Content-Length"] = std::to_string(st.st_size);
                            file_fds_[sock] = file_fd;
                            file_offsets_[sock] = 0;
                            file_sizes_[sock] = st.st_size;
                            resp.body.clear();
                            Logger::get()->debug("Large file via sendfile: {}", path);
                        }
                    }
            }else {
                resp.status_code = 404;
                resp.status_message = "Not Found";
                resp.body = "<h1>404 Not Found</h1>";
                resp.headers["Content-Type"] = "text/html";
                resp.headers["Content-Length"] = std::to_string(resp.body.size());
                resp.chunked = false;
            }
        }
    } // if (!path_handled)
after_file:
    if (keep_alive_[sock]) {
        resp.headers["Connection"] = "keep-alive";
    } 
    else {
        resp.headers["Connection"] = "close";
    }

    // ★ Uniformly add the Server header
    resp.headers["Server"] = "EpollHTTP/0.2";

    if (req.method != "HEAD" && !already_compressed && should_compress(req, resp)) {
        std::string compressed;
        if (gzip_compress(resp.body, compressed)) {
            resp.body = std::move(compressed);
            resp.headers["Content-Encoding"] = "gzip";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
        }
    }

    if (resp.chunked) {
        resp.headers.erase("Content-Length");               // Must not coexist
        resp.headers["Transfer-Encoding"] = "chunked";
        // Encode the raw body into chunked format
        std::string chunked_body;
        if (!resp.body.empty()) {
            chunked_body += to_hex(resp.body.size()) + "\r\n";
            chunked_body += resp.body + "\r\n";
        }
        chunked_body += "0\r\n\r\n";   // Terminating chunk
        resp.body = chunked_body;
    }

    // Compute the total number of bytes to send
    size_t total_bytes = 0;
    // Serialize the response headers and put them into the send queue
    std::string header_str = headers_to_string(resp);
    total_bytes += header_str.size();

    bool is_head = (req.method == "HEAD");
    if (!is_head) {   // Only non-HEAD requests may carry a body or a file
        if (!resp.body.empty()) {
            total_bytes += resp.body.size();
        } else if (resp.status_code == 200) {
            // File response: header size + file size
            total_bytes += file_sizes_[sock];   // The file size is stored by now
        }
    }
    else{
        // HEAD requests must not have a body; the fd is managed by FdCache, so just clean up the mappings
        auto file_it = file_fds_.find(sock);
        if (file_it != file_fds_.end()) {
            file_fds_.erase(sock);
            file_offsets_.erase(sock);
            file_sizes_.erase(sock);
        }
        resp.body.clear();   // Make sure the inline body is cleared
    }

    // Store the status code and total size
    resp_status_[sock] = resp.status_code;
    resp_size_[sock] = total_bytes;

    auto& queue = send_queues_[sock];
    queue.emplace_back(header_str.begin(), header_str.end());

    // If there is an inline body (error page), put it in the queue as well
    if (!resp.body.empty()) {
        queue.emplace_back(resp.body.begin(), resp.body.end());
    }

    // Enable write events
    int fd = sock->getFd();
    epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
}

void HttpHandler::send_error_response(Socket* sock, int code, const std::string& message,const std::string& allow_methods){
    HttpResponse resp = make_error_response(code, message, message);
    auto req_it = last_requests_.find(sock);
    if (req_it != last_requests_.end() && !req_it->second.trace_id.empty()) {
        resp.headers["X-Trace-Id"] = req_it->second.trace_id;
    }
    keep_alive_[sock] = false;
    resp.headers["Connection"] = "close";
    if (!allow_methods.empty()) {
        resp.headers["Allow"] = allow_methods;
    }

    std::string header = headers_to_string(resp);
    auto& queue = send_queues_[sock];
    queue.emplace_back(header.begin(), header.end());
    queue.emplace_back(resp.body.begin(), resp.body.end());

    resp_status_[sock] = code;
    resp_size_[sock] = header.size() + resp.body.size();

    int fd = sock->getFd();
    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
}

std::string HttpHandler::headers_to_string(const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_message << "\r\n";
    for (const auto& [key, value] : resp.headers) {
        oss << key << ": " << value << "\r\n";
    }
    oss << "\r\n";   // Blank line separating headers and body
    return oss.str();
}

void HttpHandler::cleanup(std::shared_ptr<Socket> sock){
    Socket* sock_ptr = sock.get();
    int fd = sock->getFd();
    epoll_.del(fd);               // Explicitly remove from epoll to avoid fd-reuse races
    sock->closeSSL();             // Graceful SSL shutdown (must precede closefd)
    sock->closefd();
    read_bufs_.erase(sock_ptr);
    send_queues_.erase(sock_ptr);
    request_ready_.erase(sock_ptr);
    parsers_.erase(sock_ptr);
    requests_.erase(sock_ptr);
    keep_alive_.erase(sock_ptr);

    resp_status_.erase(sock_ptr);
    resp_size_.erase(sock_ptr);
    // fd lifetimes are managed centrally by FdCache; only clean up the mappings here, do not close
    auto fit = file_fds_.find(sock_ptr);
    if (fit != file_fds_.end()) {
        file_fds_.erase(fit);
    }
    file_offsets_.erase(sock_ptr);
    file_sizes_.erase(sock_ptr);
    client_ip_map_.erase(sock_ptr);
    request_start_time_.erase(sock_ptr);
    last_requests_.erase(sock_ptr);
    Logger::get()->info("HttpHandler: connection closed on fd {}", fd);
}

void HttpHandler::handle_write(std::shared_ptr<Socket> sock){
    int fd = sock->getFd();
    Socket* sock_ptr = sock.get();
    try {
        // Keep processing until there is no data to send and no new request to handle
        while (true) {
            auto& queue = send_queues_[sock_ptr];

            // ==================== Phase 1: send data from the user-space queue ====================
            while (!queue.empty()) {
                auto& front = queue.front();
                bool is_ssl = sock_ptr->get_is_ssl_();
                int n = is_ssl ? sock->sslWrite(front.data(), front.size())
                               : sock->send(front.data(), front.size(), 0);
                if (n > 0) {
                    if (n == front.size()) {
                        queue.pop_front();
                    } else {
                        front.erase(front.begin(), front.begin() + n);
                        // Not fully sent, wait for the next EPOLLOUT
                        epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);      // Modify events directly: keep monitoring EPOLLOUT and continue sending the remaining data on the next writable event
                        break;  // Kernel buffer full
                    }
                } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                    break;
                } else if(n == -1 && errno == EPIPE){
                    // The client has disconnected; this is normal, just clean up the connection
                    Logger::get()->debug("Client disconnected (EPIPE) on fd {}", fd);
                    cleanup(sock);
                    return;
                }else {
                    throw_system_error("send");
                }
            }

            // ==================== Phase 2: send static files (zero-copy) ====================
            // If the queue is empty, decide whether to close the connection
            if (queue.empty()) {
                auto file_it = file_fds_.find(sock_ptr);
                if (file_it != file_fds_.end()) {
                    int file_fd = file_it->second;
                    off_t offset = file_offsets_[sock_ptr];
                    off_t remaining = file_sizes_[sock_ptr] - offset;

                    bool is_ssl = sock_ptr->get_is_ssl_();

                    if (is_ssl) {
                        // SSL connections cannot use sendfile (sendfile bypasses the OpenSSL encryption layer)
                        // The file content must be read into a user-space buffer and sent via SSL_write
                        constexpr size_t SSL_SENDFILE_BUF = 65536;  // 64KB buffer
                        std::vector<char> filebuf(std::min(static_cast<off_t>(SSL_SENDFILE_BUF), remaining));
                        ssize_t read_n = ::pread(file_fd, filebuf.data(), filebuf.size(), offset);
                        if (read_n > 0) {
                            int write_n = sock->sslWrite(filebuf.data(), read_n);
                            if (write_n > 0) {
                                offset += write_n;
                                remaining -= write_n;
                                // Partial write, wait for the next EPOLLOUT
                                if (write_n < read_n) {
                                    file_offsets_[sock_ptr] = offset;
                                    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                                    return;
                                }
                            } else if (write_n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                file_offsets_[sock_ptr] = offset;
                                epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                                return;
                            } else {
                                Logger::get()->error("SSL_write for file failed on fd {}", fd);
                                file_fds_.erase(sock_ptr);
                                file_offsets_.erase(sock_ptr);
                                file_sizes_.erase(sock_ptr);
                                cleanup(sock);
                                return;
                            }
                        } else if (read_n == -1) {
                            Logger::get()->error("pread for SSL file failed: {}", strerror(errno));
                            file_fds_.erase(sock_ptr);
                            file_offsets_.erase(sock_ptr);
                            file_sizes_.erase(sock_ptr);
                            cleanup(sock);
                            return;
                        }
                        // read_n == 0 means EOF, treat as done
                    } else {
                        // Non-SSL: use sendfile zero-copy
                        while (remaining > 0) {
                            ssize_t n = sendfile(fd, file_fd, &offset, remaining);
                            if (n > 0) {
                                remaining -= n;
                            } else if (n == -1) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    file_offsets_[sock_ptr] = offset;
                                    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                                    return;
                                } else {
                                    Logger::get()->error("sendfile failed: {}", strerror(errno));
                                    file_fds_.erase(sock_ptr);
                                    file_offsets_.erase(sock_ptr);
                                    file_sizes_.erase(sock_ptr);
                                    cleanup(sock);
                                    return;
                                }
                            }
                        }
                    }

                    // File fully sent, clean up the file descriptor and mappings
                    //close(file_fd);
                    file_fds_.erase(sock_ptr);
                    file_offsets_.erase(sock_ptr);
                    file_sizes_.erase(sock_ptr);
                }

                auto status_it = resp_status_.find(sock_ptr);
                auto start_it = request_start_time_.find(sock_ptr);
                if (status_it != resp_status_.end() && start_it != request_start_time_.end()) {
                    double duration = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - start_it->second).count();
                    Metrics::instance().record_request(status_it->second, duration);
                    std::string route_name = "-";
                    auto req_it = last_requests_.find(sock_ptr);
                    if (req_it != last_requests_.end()) {
                        const auto& req = req_it->second;
                        if (req.headers.count("x-route-name") != 0) {
                            route_name = req.headers.at("x-route-name");
                        } else if (req.path.rfind("/", 0) == 0) {
                            route_name = req.path;
                        }
                        Metrics::instance().record_route_request(route_name, req.host, req.tenant, status_it->second, duration);
                    }
                    // Note: we do not erase start_time here because the CLF log below may still need it; it could be erased after CLF logging instead.
                }

                // Write the audit log once after the response has actually been sent, avoiding duplicate records in the auth/rate-limit/routing branches.
                auto audit_request_it = last_requests_.find(sock_ptr);
                if (audit_request_it != last_requests_.end() && status_it != resp_status_.end() &&
                    status_it->second >= 400) {
                    const auto& audit_request = audit_request_it->second;
                    const auto route_it = audit_request.headers.find("x-route-name");
                    const std::string route_name = route_it == audit_request.headers.end() ? "-" : route_it->second;
                    emit_audit_log(audit_request,
                                   status_it->second,
                                   classify_failure_reason(status_it->second),
                                   route_name,
                                   audit_request.host.empty() ? "-" : audit_request.host,
                                   audit_request.tenant.empty() ? "-" : audit_request.tenant);
                }

                // ★ New: record the access log in CLF format
                {
                    auto ip_it = client_ip_map_.find(sock_ptr);
                    std::string client_ip = (ip_it != client_ip_map_.end()) ? ip_it->second : "-";

                    // Safely fetch the request duration (guard against the map having been cleared by cleanup, which would invalidate iterators)
                    int64_t duration_us = 0;
                    auto start_it = request_start_time_.find(sock_ptr);
                    if (start_it != request_start_time_.end()) {
                        duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start_it->second).count();
                    }

                    std::string method, path, version, user_agent;
                    auto req_it = last_requests_.find(sock_ptr);
                    if (req_it != last_requests_.end()) {
                        method = req_it->second.method;
                        path = req_it->second.path;
                        version = req_it->second.version;
                        auto ua = req_it->second.headers.find("user-agent");
                        user_agent = (ua != req_it->second.headers.end()) ? ua->second : "-";
                        last_requests_.erase(sock_ptr);
                    }

                    char time_buf[64];
                    time_t now = time(nullptr);
                    strftime(time_buf, sizeof(time_buf), "%d/%b/%Y:%H:%M:%S %z", localtime(&now));

                    // Safely fetch the status code and response size
                    int status_code = 0;
                    size_t response_size = 0;
                    auto status_it = resp_status_.find(sock_ptr);
                    if (status_it != resp_status_.end()) status_code = status_it->second;
                    auto size_it = resp_size_.find(sock_ptr);
                    if (size_it != resp_size_.end()) response_size = size_it->second;

                    Logger::get()->info("{} - - [{}] \"{} {} {}\" {} {} \"-\" \"{}\" {}us",
                        client_ip, time_buf, method, path, version,
                        status_code, response_size,
                        user_agent, duration_us);
                }

                // Clean up the temporary records for this response
                resp_status_.erase(sock_ptr);
                resp_size_.erase(sock_ptr);

                // If the connection should be closed, clean up and return
                if (!keep_alive_[sock_ptr]) {
                    cleanup(sock);
                    return;
                }

                // Check whether the read buffer already holds a pending request
                if (!read_bufs_[sock_ptr].empty()) {
                    std::string ip = client_ip_map_[sock_ptr]; // Reuse the IP
                    handle_read(sock, ip);
                    // Loop again to try sending the newly generated data
                    continue;
                }

                // No pending requests left: switch to read events and break out of the loop
                epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
                break;
            } else {
                // The send queue still has data (EAGAIN or partial send), wait for the next EPOLLOUT
                epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                return;
            }
        }
    }
    catch (const std::exception& e) {
        Logger::get()->error("Exception in handle_write: {}", e.what());
        cleanup(sock);
    } catch (...) {
        Logger::get()->error("Unknown exception in handle_write");
        cleanup(sock);
    }
}

void HttpHandler::close_connection(std::shared_ptr<Socket> sock){
    cleanup(sock);
}

void HttpHandler::addRoute(const std::string& method, const std::string& pattern, RouteHandler handler) {
    GatewayRoute route;
    route.method = method;
    route.path = pattern;
    route.target_type = "local";
    route.enabled = true;
    route.auth_required = false;
    route.allow_anonymous = true;
    route.rate_limit_policy = "route";
    route.host = "*";
    route.tenant = "*";
    routes_.push_back({route, handler});
}

void HttpHandler::addRoute(const GatewayRoute& route, RouteHandler handler) {
    GatewayRoute route_policy = route;
    route_policy.target_type = route_policy.target_type.empty() ? "local" : route_policy.target_type;
    route_policy.host = route_policy.host.empty() ? "*" : route_policy.host;
    route_policy.tenant = route_policy.tenant.empty() ? "*" : route_policy.tenant;
    routes_.push_back({route_policy, std::move(handler)});
}

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

bool HttpHandler::requires_auth(const HttpRequest& req) const {
    // Only /api/-prefixed endpoints require auth; static files and public routes like /metrics need none
    return req.path.rfind("/api/", 0) == 0;
}

bool HttpHandler::should_rate_limit(const HttpRequest& req, const GatewayRoute& route, const std::string& client_ip, const ApiKeyConfig* api_key_cfg) const {
    if (route.rate_limit_policy == "route") {
        return !rate_limiter_manager_->try_acquire("route:" + route.path + ":" + req.method, config_.rate_limit_config);
    }
    if (route.rate_limit_policy == "api_key" && api_key_cfg) {
        return !rate_limiter_manager_->try_acquire("api_key:" + api_key_cfg->key, api_key_cfg->rate_limit);
    }
    return !rate_limiter_manager_->try_acquire("ip:" + client_ip, config_.rate_limit_config);
}

std::string HttpHandler::extract_api_key(const HttpRequest& req){
    // 1. Prefer the X-API-Key header
    auto it = req.headers.find("x-api-key");
    if (it != req.headers.end()) {
        return it->second;
    }
    // 2. Fall back to Authorization: Bearer <key>
    auto auth = req.headers.find("authorization");
    if (auth != req.headers.end()) {
        const std::string& val = auth->second;
        if (val.rfind("Bearer ", 0) == 0) {
            return val.substr(7);
        }
    }
    return "";
}