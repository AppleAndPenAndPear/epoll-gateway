#include "http_handler.h"
#include <sys/epoll.h>
#include <sys/stat.h>       // needed for struct stat
#include "content_type.h"
#include <sys/sendfile.h>       // needed for sendfile
#include <sys/uio.h>           // needed for writev/iovec (plaintext gather send)
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
#include "logging_utils.h"
#include "gateway_version.h"
#include <atomic>
#include <cstdint>
#include <cstdio>              // needed for std::snprintf
#include <functional>
#include <unordered_set>
using json = nlohmann::json;

namespace {

std::atomic<uint64_t> g_trace_counter{0};
std::atomic<uint64_t> g_traceparent_counter{0};

// ASCII-only case mapping for header names: locale-independent, branchless
// enough, and no libc call per character (profiled hot: std::tolower in the
// per-response header scan). Bytes >= 0x80 pass through unchanged, which keeps
// the historical std::tolower-with-unsigned-char semantics for ASCII input.
inline char ascii_to_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
}

// Case-insensitive ASCII comparison of a header name against a lowercase
// literal, without allocating a lowercase copy of the name.
bool iequals_ascii_lower(const std::string& name, const char* lower_literal) {
    size_t i = 0;
    for (; i < name.size() && lower_literal[i] != '\0'; ++i) {
        if (ascii_to_lower(static_cast<unsigned char>(name[i])) != lower_literal[i]) return false;
    }
    return i == name.size() && lower_literal[i] == '\0';
}

// Hop-by-hop response headers that must be consumed by the gateway and never
// forwarded to the client (RFC 7230 §6.1); Content-Length/Transfer-Encoding
// are re-framed by the gateway itself.
bool is_response_hop_by_hop(const std::string& lower_name) {
    static const std::unordered_set<std::string> kHopByHop = {
        "content-length", "transfer-encoding", "connection", "keep-alive",
        "proxy-authenticate", "proxy-authorization", "proxy-connection",
        "te", "trailer", "upgrade"};
    return kHopByHop.count(lower_name) != 0;
}

// Tokens the backend named in its own Connection header are hop-by-hop too.
std::unordered_set<std::string> response_connection_tokens(
        const std::map<std::string, std::string>& headers) {
    std::unordered_set<std::string> tokens;
    // Only the Connection header contributes tokens; find it with a
    // case-insensitive compare instead of allocating a lowercase copy of every
    // header name (profiled hot on the proxy path).
    const std::string* conn_value = nullptr;
    for (const auto& entry : headers) {
        if (iequals_ascii_lower(entry.first, "connection")) {
            conn_value = &entry.second;
            break;
        }
    }
    if (conn_value == nullptr) return tokens;
    // Split on commas without istringstream; trim and lowercase in place.
    const std::string& value = *conn_value;
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t comma = value.find(',', pos);
        if (comma == std::string::npos) comma = value.size();
        size_t begin = pos;
        size_t end = comma;
        while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
        while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) --end;
        if (begin < end) {
            std::string token = value.substr(begin, end - begin);
            for (char& c : token) c = ascii_to_lower(static_cast<unsigned char>(c));
            tokens.insert(std::move(token));
        }
        pos = comma + 1;
    }
    return tokens;
}

// Adds the client-identity headers on the upstream hop. Shared by the plain
// upstream and the composite routes so the two cannot drift apart.
// All header names here are lowercase because the request parser stores every
// header key lowercased. Writing a mixed-case name would create a *second*
// map entry next to a client-supplied one and let the client spoof the value.
void inject_forwarded_headers(std::unordered_map<std::string, std::string>& headers,
                              const std::string& client_ip) {
    if (!client_ip.empty()) {
        auto xff = headers.find("x-forwarded-for");
        if (xff != headers.end()) xff->second += ", " + client_ip;
        else headers["x-forwarded-for"] = client_ip;
        // Overwrite, never append: a client-supplied X-Real-IP must not survive.
        headers["x-real-ip"] = client_ip;
    }
    // The client-facing listener is always TLS (config validation requires a
    // cert/key pair), so the original request scheme is https. Overwriting any
    // client-supplied value keeps backends from trusting a spoofed scheme.
    headers["x-forwarded-proto"] = "https";
    // Preserve the client's original Host for backends that route by vhost; the
    // Host header on the upstream hop itself stays the backend host:port.
    const auto host_it = headers.find("host");
    if (host_it != headers.end() && !host_it->second.empty()) {
        headers["x-forwarded-host"] = host_it->second;
    }
}

std::string generate_trace_id() {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const uint64_t seq = ++g_trace_counter;
    // snprintf instead of ostringstream (profiled hot on the request path).
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%08llx%08llx",
                  static_cast<unsigned long long>(now_ns & 0xffffffffULL),
                  static_cast<unsigned long long>(seq & 0xffffffffULL));
    return std::string(buf, 16);
}

std::string normalize_trace_id_for_traceparent(const std::string& trace_id) {
    std::string normalized = trace_id.empty() ? generate_trace_id() : trace_id;
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](unsigned char ch) {
        return !std::isxdigit(ch);
    }), normalized.end());
    if (normalized.size() > 32) {
        normalized = normalized.substr(0, 32);
    }
    while (normalized.size() < 32) {
        normalized = "0" + normalized;
    }
    return normalized;
}

std::string generate_traceparent(const std::string& trace_id = "", const std::string& dep_name = "") {
    const std::string normalized_trace_id = normalize_trace_id_for_traceparent(trace_id.empty() ? generate_trace_id() : trace_id);
    const uint64_t parent_id_seed = std::hash<std::string>{}(dep_name.empty() ? std::string("dependency") : dep_name + ":" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const uint64_t parent_id = parent_id_seed & 0xffffffffffffffffULL;
    // snprintf instead of ostringstream (profiled hot on the request path).
    char buf[64];
    std::snprintf(buf, sizeof(buf), "00-%s-%016llx-01",
                  normalized_trace_id.c_str(),
                  static_cast<unsigned long long>(parent_id));
    return std::string(buf);
}

bool is_hex_string(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

std::string parse_trace_id_from_traceparent(const std::string& traceparent) {
    if (traceparent.size() < 35) {
        return {};
    }
    const auto first_dash = traceparent.find('-');
    if (first_dash == std::string::npos) {
        return {};
    }
    const auto second_dash = traceparent.find('-', first_dash + 1);
    if (second_dash == std::string::npos) {
        return {};
    }
    const std::string trace_id = traceparent.substr(first_dash + 1, second_dash - first_dash - 1);
    if (trace_id.size() == 32 && is_hex_string(trace_id)) {
        return trace_id;
    }
    return {};
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
    if (status_code == 502) return "upstream_unavailable";
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

void emit_audit_log(const HttpRequest& req, int status_code, const std::string& failure_reason, const std::string& route_name, const std::string& host, const std::string& tenant, const LoggingConfig& logging_config) {
    const std::string trace_id = req.trace_id.empty() ? "unknown" : req.trace_id;
    const std::string key = req.headers.count("x-api-key") ? req.headers.at("x-api-key") : "";
    const std::string api_key = logging_utils::redact_api_key(key);
    const std::string user_agent = req.headers.count("user-agent") ? req.headers.at("user-agent") : "-";
    const std::string auth_header = req.headers.count("authorization") ? req.headers.at("authorization") : "";
    const std::string cookie_header = req.headers.count("cookie") ? req.headers.at("cookie") : "";
    Logger::get()->info(
        "AUDIT trace_id={} method={} path={} host={} tenant={} route={} status={} failure={} api_key={} user_agent={} authorization={} cookie={}",
        trace_id,
        req.method,
        logging_utils::redact_request_target(req.path, req.query, logging_config),
        host,
        tenant,
        route_name,
        status_code,
        failure_reason,
        api_key,
        logging_utils::redact_header_value("user-agent", user_agent, logging_config),
        logging_utils::redact_header_value("authorization", auth_header, logging_config),
        logging_utils::redact_header_value("cookie", cookie_header, logging_config));
}

} // anonymous namespace

static std::string to_hex(size_t n) {
    std::ostringstream oss;
    oss << std::hex << n;
    return oss.str();
}

// A composite dependency that never reached a backend is resolved inline; the
// event loop only ever drives dependencies with a live UpstreamExchange.
static std::int64_t composite_elapsed_ms(std::chrono::steady_clock::time_point started) {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
}

HttpHandler::HttpHandler(Epoll& epoll, const Config& config, UpstreamManager& upstream_manager, ApiKeyManager& api_key_manager, std::shared_ptr<RateLimiterManager> rate_limiter_manager) : epoll_(epoll), www_root_(config.www_root), config_(config), 
cache_(config.cache_max_entries, config.cache_max_file_size_mb), 
upstream_manager_(upstream_manager),
    api_key_manager_(api_key_manager), rate_limiter_manager_(std::move(rate_limiter_manager)) {
    register_default_routes();
    register_configured_routes();
}

HttpHandler::~HttpHandler() = default;

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
    // ── Operations endpoints (P4). Probe endpoints must never be rate limited
    // or fail because of one — see should_rate_limit(). ──
    addRoute("GET", "/healthz", [](const HttpRequest& req, HttpResponse& resp, const RouteParams&) {
        // Liveness: the process is up and its event loop is serving requests.
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "text/plain";
        resp.body = "ok\n";
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
    });

    addRoute("GET", "/readyz", [this](const HttpRequest& req, HttpResponse& resp, const RouteParams&) {
        // Readiness: every configured upstream must have at least one healthy
        // backend. A gateway that cannot reach any backend should be removed
        // from the load balancer pool instead of failing client requests.
        const auto snapshot = upstream_manager_.health_snapshot();
        bool ready = true;
        json upstreams = json::object();
        for (const auto& [name, counts] : snapshot) {
            upstreams[name] = {{"healthy", counts.first}, {"total", counts.second}};
            if (counts.first == 0) ready = false;
        }
        json j;
        j["status"] = ready ? "ready" : "unavailable";
        j["upstreams"] = upstreams;
        std::string body = j.dump() + "\n";
        resp.status_code = ready ? 200 : 503;
        resp.status_message = ready ? "OK" : "Service Unavailable";
        resp.headers["Content-Type"] = "application/json";
        resp.body = body;
        resp.headers["Content-Length"] = std::to_string(body.size());
    });

    addRoute("GET", "/version", [](const HttpRequest& req, HttpResponse& resp, const RouteParams&) {
        json j;
        j["version"] = GATEWAY_VERSION;
        std::string body = j.dump() + "\n";
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = body;
        resp.headers["Content-Length"] = std::to_string(body.size());
    });

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
            auto& read_buf = read_bufs_[sock_ptr];
            const bool has_buffered_data = !read_buf.empty();
            int n = 0;
            if (!has_buffered_data) {
                bool is_ssl = sock_ptr->get_is_ssl_();
                // sslRead counts a bare FIN (no close_notify) as a clean end of stream,
                // which is what most HTTP clients do: request completeness comes from
                // HTTP framing, so the close style must not be reported as a read error.
                n = is_ssl ? sock_ptr->sslRead(buf, sizeof(buf)) : sock_ptr->recv(buf, sizeof(buf), 0);
                if (n > 0) read_buf.append(buf, n);
            }
            if (has_buffered_data || n > 0) {
                // Keep parsing the buffer until no complete request remains
                while (true) {
                    size_t consumed = 0;
                    const bool parsed = parsers_[sock_ptr].parse(
                        read_buf.data(), read_buf.size(), requests_[sock_ptr], consumed);
                    if (consumed > 0) read_buf.erase(0, consumed);
                    if (parsed) {
                        // Parsed a complete request; record the total request count
                        Metrics::instance().record_total_request();

                        // Get a reference to the parsed request
                        auto& req = requests_[sock_ptr];

                        const auto host_it = req.headers.find("host");
                        const auto tenant_it = req.headers.find("x-tenant-id");
                        if (host_it != req.headers.end()) req.host = host_it->second;
                        if (tenant_it != req.headers.end()) req.tenant = tenant_it->second;
                        if (req.trace_id.empty()) {
                            if (req.headers.count("x-trace-id")) {
                                req.trace_id = req.headers["x-trace-id"];
                            } else if (req.headers.count("traceparent")) {
                                req.trace_id = parse_trace_id_from_traceparent(req.headers["traceparent"]);
                                if (req.trace_id.empty()) {
                                    req.trace_id = generate_trace_id();
                                }
                            } else {
                                req.trace_id = generate_trace_id();
                            }
                        }
                        if (req.headers.count("traceparent") == 0) {
                            req.headers["traceparent"] = generate_traceparent();
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
                            send_response(sock, req, resolved);
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
                            // ::tolower is UB for bytes >= 0x80; go through unsigned char (project convention).
                            std::transform(conn.begin(), conn.end(), conn.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            keep_alive_[sock_ptr] = (conn != "close");
                        } else {
                            keep_alive_[sock_ptr] = true; // HTTP/1.1 defaults to keep-alive
                        }

                        // Prepare the response (using the request parsed above)
                        send_response(sock, req, resolved);

                        // Reset the parser for the next request
                        parsers_[sock_ptr].reset();

                        // An upstream route is now being driven by the event loop.
                        // Leave the fd disarmed (EPOLLONESHOT already fired) and
                        // stop parsing further pipelined requests on this connection:
                        // the child completion path frames and sends the response,
                        // then the write path picks up any buffered follow-up request.
                        if (client_upstream_.count(sock_ptr)) {
                            return;
                        }

                        // ★ Key: break out of the inner loop; handle the next request after sending completes
                        break;
                    }
                    // ─── Reject malformed requests (request smuggling prevention) ───
                    if (parsers_[sock_ptr].error() != HttpParser::ParseError::NONE) {
                        const HttpParser::ParseError err = parsers_[sock_ptr].error();
                        if (err == HttpParser::ParseError::BODY_TOO_LARGE) {
                            Logger::get()->warn("Request body too large (>{}) on fd {}",
                                HttpParser::MAX_BODY_SIZE, fd);
                            last_requests_[sock_ptr] = requests_[sock_ptr];
                            send_error_response(sock_ptr, 413, "Payload Too Large");
                        } else {
                            Logger::get()->warn("Malformed request rejected on fd {}: error={}", fd, static_cast<int>(err));
                            last_requests_[sock_ptr] = requests_[sock_ptr];
                            send_error_response(sock_ptr, 400, "Bad Request");
                        }
                        // Stop parsing: after a smuggling-class rejection the
                        // rest of the buffer is untrusted, and send_error_response
                        // already queued "Connection: close", so nothing further
                        // on this connection should be handled.
                        parsers_[sock_ptr].reset();
                        requests_[sock_ptr].clear();
                        break;
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
        // Re-arm read events (unless an upstream round trip is in flight on this
        // connection, in which case rearm_reads() leaves the fd disarmed).
        rearm_reads(sock_ptr);
    } catch (const std::exception& e) {
        Logger::get()->error("Exception in handle_read: {}", e.what());
        cleanup(sock);
    } catch (...) {
        Logger::get()->error("Unknown exception in handle_read");
        cleanup(sock);
    }
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
    // Host/tenant are loop-invariant: look them up once, not per route.
    const auto host_it = req.headers.find("host");
    const auto tenant_it = req.headers.find("x-tenant-id");
    const std::string host = host_it == req.headers.end() ? "" : host_it->second;
    const std::string tenant = tenant_it == req.headers.end() ? "" : tenant_it->second;
    for (const auto& registered : routes_) {
        RouteParams params;
        // Path/host/tenant match once per route; the method test below reuses
        // the result instead of re-running the whole path match (routeMatchesPath
        // used to be evaluated twice per route, per request).
        if (routeMatchesPath(registered.route, host, tenant, req.path, params)) {
            matched.path_matched = true;
            const std::string& method = registered.route.method;
            if (!matched.allow_methods.empty()) {
                matched.allow_methods += ", ";
            }
            matched.allow_methods += method.empty() ? "*" : method;
            if (method.empty() || method == "*" || method == req.method) {
                matched.route = &registered;
                matched.params = std::move(params);
                return matched;
            }
        }
    }
    return matched;
}

void HttpHandler::dispatch_route(const HttpRequest& req, const ResolvedRoute& matched, HttpResponse& resp) {
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

    if (matched.route->route.target_type == "composite") {
        // Unreachable in practice: send_response() intercepts composite routes
        // and dispatches them through start_composite_exchange() (each dependency
        // as its own event-loop driven UpstreamExchange) before calling here.
        // Kept as a defensive fallback so a direct caller still gets a framed
        // response instead of an empty one.
        resp = make_error_response(500, "Internal Server Error", "composite route dispatched synchronously");
        return;
    }

    if (matched.route->route.target_type == "upstream") {
        // Unreachable in practice: send_response() intercepts upstream routes
        // and dispatches them through start_upstream_exchange() (the event-loop
        // driven UpstreamExchange) before calling here. Kept as a defensive
        // fallback so a direct caller still gets a framed response instead of
        // an empty one.
        resp = make_error_response(500, "Internal Server Error", "upstream route dispatched synchronously");
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

void HttpHandler::send_response(std::shared_ptr<Socket> sock, const HttpRequest& req, const ResolvedRoute& matched){
    Socket* sock_ptr = sock.get();
    last_requests_[sock_ptr] = req;
    HttpResponse resp;
    resp.headers["X-Trace-Id"] = req.trace_id.empty() ? generate_trace_id() : req.trace_id;
    resp.headers["Traceparent"] = req.headers.count("traceparent") ? req.headers.at("traceparent") : generate_traceparent();
    std::string path = req.path;

    // Default index page
    if (path.empty() || path == "/") path = "/index.html"; // Default index page

    bool path_handled = false;

    if (matched.route) {
        if (matched.route->route.target_type == "upstream") {
            // Upstream routes are dispatched through the event loop: the
            // exchange drives the backend socket from this same epoll, so the
            // call consumes no worker thread while it waits. The response is
            // framed later, when the exchange finishes.
            start_upstream_exchange(std::move(sock), req, matched);
            return;
        }
        if (matched.route->route.target_type == "composite") {
            // Composite routes are dispatched through the event loop too: every
            // dependency is driven as its own non-blocking UpstreamExchange, so
            // the fan-out consumes no worker thread while it waits. The aggregate
            // is framed later, when the last dependency finishes or the overall
            // deadline fires.
            auto ip_it = client_ip_map_.find(sock_ptr);
            const std::string client_ip = (ip_it != client_ip_map_.end()) ? ip_it->second : "";
            start_composite_exchange(std::move(sock), req, matched, client_ip);
            return;
        }
        dispatch_route(req, matched, resp);
        resp.headers["X-Trace-Id"] = req.trace_id.empty() ? generate_trace_id() : req.trace_id;
        if (resp.headers.count("Traceparent") == 0) {
            resp.headers["Traceparent"] = req.headers.count("traceparent") ? req.headers.at("traceparent") : generate_traceparent();
        }
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
        // Path traversal guard, shared with the "static" route type: canonicalize
        // the joined path and require it to stay inside the document root. Using
        // the same helper in both places keeps the two checks from drifting.
        else if (std::string resolved_static;
                 !is_safe_static_path("/", path, www_root_, resolved_static)) {
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
                    finalize_response(sock_ptr, req, resp, false);
                    return;
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
                        finalize_response(sock_ptr, req, resp, false);
                        return;
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
                                    finalize_response(sock_ptr, req, resp, false);
                                    return;
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
                                    finalize_response(sock_ptr, req, resp, false);
                                    return;
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
                            file_fds_[sock_ptr] = file_fd;
                            file_offsets_[sock_ptr] = 0;
                            file_sizes_[sock_ptr] = st.st_size;
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

    finalize_response(sock_ptr, req, resp, already_compressed);
}

void HttpHandler::finalize_response(Socket* sock, const HttpRequest& req, HttpResponse& resp, bool already_compressed) {
    // Ensure the trace-correlation headers are present even for responses built
    // off the event loop (upstream completions) that never ran the
    // send_response() prologue.
    if (resp.headers.count("X-Trace-Id") == 0) {
        resp.headers["X-Trace-Id"] = req.trace_id.empty() ? generate_trace_id() : req.trace_id;
    }
    if (resp.headers.count("Traceparent") == 0) {
        resp.headers["Traceparent"] = req.headers.count("traceparent") ? req.headers.at("traceparent") : generate_traceparent();
    }

    if (keep_alive_[sock]) {
        resp.headers["Connection"] = "keep-alive";
    } 
    else {
        resp.headers["Connection"] = "close";
    }

    // ★ Uniformly add the Server header, sourced from the same version constant
    // that /version and the admin endpoints report, so the two can never drift.
    resp.headers["Server"] = std::string("EpollHTTP/") + GATEWAY_VERSION;

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
    // Queue the header as a moved buffer (no copy). On TLS the body is coalesced
    // into it first: SSL_write needs contiguous plaintext (OpenSSL has no writev
    // variant), so one buffer means one SSL_write / one record. On plaintext the
    // body stays a separate chunk and the send path writev()s both in a single
    // syscall. Large TLS bodies stay split so peak memory stays bounded. HEAD
    // requests have no body here (cleared above).
    constexpr size_t kMaxCoalesceBytes = 64 * 1024;
    const bool has_body = !is_head && !resp.body.empty();
    if (has_body && sock->get_is_ssl_() && resp.body.size() <= kMaxCoalesceBytes) {
        header_str.append(resp.body);
        queue.emplace_back(std::move(header_str));
    } else {
        queue.emplace_back(std::move(header_str));
        if (has_body) {
            queue.emplace_back(resp.body.begin(), resp.body.end());
        }
    }

    // Enable write events
    int fd = sock->getFd();
    epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
}

void HttpHandler::start_upstream_exchange(std::shared_ptr<Socket> sock, const HttpRequest& req, const ResolvedRoute& matched) {
    const GatewayRoute& route = matched.route->route;
    const std::string upstream_key = resolve_upstream_group_id(route);
    Socket* sock_ptr = sock.get();

    // Snapshot every value the exchange needs while we are still on the
    // event-loop thread: `config_` and `routes_` are reassigned in place on a
    // hot reload, and `matched.route` points into `routes_`, so nothing may be
    // read once control returns to the loop.
    const auto up_it = config_.upstream_config.upstreams.find(upstream_key);
    const bool has_upstream =
        up_it != config_.upstream_config.upstreams.end() && !up_it->second.servers.empty();
    if (!has_upstream) {
        // A route whose upstream group has no servers: answer inline, like the
        // former synchronous overload/no-server paths did.
        HttpResponse resp = make_error_response(502, "Bad Gateway", "Bad Gateway: no upstream server");
        finalize_response(sock_ptr, req, resp, false);
        return;
    }

    const auto ip_it = client_ip_map_.find(sock_ptr);
    const std::string client_ip = (ip_it != client_ip_map_.end()) ? ip_it->second : "";
    // Copy the picked server: pick_server() returns a reference into the
    // manager's config, which reload() can replace underneath us.
    const UpstreamServer server = upstream_manager_.pick_server(upstream_key);
    const int circuit_recovery_timeout_ms = route.upstream_target.circuit_recovery_timeout_ms;
    if (!upstream_manager_.allow_request(upstream_key, server, circuit_recovery_timeout_ms)) {
        HttpResponse resp = make_error_response(503, "Service Unavailable", "Upstream circuit is open");
        finalize_response(sock_ptr, req, resp, false);
        return;
    }

    auto st = std::make_unique<ExchangeState>();
    st->client = sock;
    st->req = req;

    auto child = std::make_unique<ChildExchange>();
    child->server = server;
    child->upstream_key = upstream_key;
    child->max_retries = route.upstream_target.max_retries;
    child->circuit_failure_threshold = route.upstream_target.circuit_failure_threshold;
    child->circuit_recovery_timeout_ms = circuit_recovery_timeout_ms;
    child->params.host = server.host;
    child->params.port = server.port;
    child->params.method = req.method;
    child->params.path = req.path;
    child->params.headers = req.headers;
    child->params.body = req.body;
    child->params.timeout_ms = route.upstream_target.timeout_ms;
    child->params.max_response_bytes = config_.max_response_bytes;
    child->params.tls.enable = server.tls;
    child->params.tls.ca_file = server.tls_ca_file;
    child->params.tls.server_name = server.tls_server_name;
    child->params.tls.skip_verify = server.tls_skip_verify;
    if (child->params.headers.count("traceparent") == 0) {
        child->params.headers["traceparent"] = generate_traceparent();
    }
    inject_forwarded_headers(child->params.headers, client_ip);

    const bool idempotent = child->params.method == "GET" || child->params.method == "HEAD" ||
                            child->params.method == "OPTIONS";
    child->ex = std::make_unique<UpstreamExchange>(child->params, idempotent);
    st->children.push_back(std::move(child));

    // Publish the exchange BEFORE driving it: the very first step can finish
    // synchronously (immediate failure) and the completion path looks it up
    // through this map, so it must already be present.
    client_upstream_[sock_ptr] = std::move(st);
    drive_exchange(sock_ptr, 0);
}

void HttpHandler::start_composite_exchange(std::shared_ptr<Socket> sock, const HttpRequest& req,
                                           const ResolvedRoute& matched, const std::string& client_ip) {
    Socket* sock_ptr = sock.get();

    // Snapshot the whole dependency list, the route name and the optional overall
    // deadline while we are still on the event-loop thread: after this returns we
    // are back on the loop, and a hot reload clears and rebuilds `routes_`, so
    // `matched.route` must not be read again.
    const GatewayRoute& route = matched.route->route;
    auto st = std::make_unique<ExchangeState>();
    st->client = sock;
    st->req = req;
    st->client_ip = client_ip;
    st->composite = true;
    st->route_name = route.name;
    st->composite_name = route.composite_target.name;
    st->started = std::chrono::steady_clock::now();
    st->total_timeout_ms = route.composite_target.total_timeout_ms;
    st->has_total_deadline = st->total_timeout_ms > 0;
    st->total_deadline = st->started + std::chrono::milliseconds(st->total_timeout_ms);

    const int failure_threshold = route.upstream_target.circuit_failure_threshold;
    const std::size_t max_response_bytes = config_.max_response_bytes;
    std::vector<std::size_t> live;

    for (const auto& dep : route.composite_target.dependencies) {
        auto child = std::make_unique<ChildExchange>();
        child->name = dep.name;
        child->required = dep.required;
        child->upstream_key = dep.upstream_name;

        json item = json::object();
        item["required"] = dep.required;
        item["upstream"] = dep.upstream_name;
        item["method"] = dep.method;
        item["path"] = dep.path;
        item["trace_id"] = req.trace_id.empty() ? generate_trace_id() : req.trace_id;
        item["traceparent"] = generate_traceparent(item["trace_id"].get<std::string>(),
                                                   dep.name.empty() ? dep.upstream_name : dep.name);
        item["attempts"] = 1;
        item["timed_out"] = false;
        child->item = std::move(item);

        const auto config_it = config_.upstream_config.upstreams.find(dep.upstream_name);
        if (dep.upstream_name.empty() ||
            config_it == config_.upstream_config.upstreams.end() ||
            config_it->second.servers.empty()) {
            child->item["status"] = "unavailable";
            child->item["error"] = dep.required ? "missing_upstream" : "optional_dependency_missing";
            child->item["duration_ms"] = composite_elapsed_ms(st->started);
            child->finished = true;
            Logger::get()->info("COMPOSITE dependency={} status={} required={} traceparent={} duration_ms={}",
                                dep.name.empty() ? "<unnamed>" : dep.name, "unavailable", dep.required,
                                child->item["traceparent"].get<std::string>(),
                                child->item["duration_ms"].get<std::int64_t>());
            st->children.push_back(std::move(child));
            continue;
        }

        // Copy the picked server: pick_server() returns a reference into the
        // manager's config, which reload() can replace underneath us.
        const UpstreamServer server = upstream_manager_.pick_server(dep.upstream_name);
        child->server = server;
        child->circuit_failure_threshold = failure_threshold;
        // Recovery timeout mirrors the former allow_request() call, which used
        // the dependency's own timeout; kept as-is for behavioural parity.
        child->circuit_recovery_timeout_ms = dep.timeout_ms;
        if (!upstream_manager_.allow_request(dep.upstream_name, server, dep.timeout_ms)) {
            child->item["status"] = "failed";
            child->item["http_status"] = 503;
            child->item["error"] = "circuit_open";
            child->item["duration_ms"] = composite_elapsed_ms(st->started);
            child->finished = true;
            Logger::get()->info("COMPOSITE dependency={} status={} required={} traceparent={} duration_ms={} http_status={}",
                                dep.name.empty() ? "<unnamed>" : dep.name, "failed", dep.required,
                                child->item["traceparent"].get<std::string>(),
                                child->item["duration_ms"].get<std::int64_t>(), 503);
            st->children.push_back(std::move(child));
            continue;
        }

        // Deadline propagation: the backend call may not outlive the overall
        // budget, so it gets the smaller of its own timeout and the budget.
        const int effective_timeout_ms = st->has_total_deadline
            ? std::min(dep.timeout_ms, st->total_timeout_ms)
            : dep.timeout_ms;
        child->params.host = server.host;
        child->params.port = server.port;
        child->params.method = dep.method;
        child->params.path = dep.path;
        child->params.headers = req.headers;
        child->params.body = req.body;
        child->params.timeout_ms = effective_timeout_ms;
        child->params.max_response_bytes = max_response_bytes;
        child->params.tls.enable = server.tls;
        child->params.tls.ca_file = server.tls_ca_file;
        child->params.tls.server_name = server.tls_server_name;
        child->params.tls.skip_verify = server.tls_skip_verify;
        if (child->params.headers.count("traceparent") == 0) {
            child->params.headers["traceparent"] = child->item["traceparent"].get<std::string>();
        }
        inject_forwarded_headers(child->params.headers, client_ip);

        const bool idempotent = dep.method == "GET" || dep.method == "HEAD" || dep.method == "OPTIONS";
        child->ex = std::make_unique<UpstreamExchange>(child->params, idempotent);
        live.push_back(st->children.size());
        st->children.push_back(std::move(child));
    }

    client_upstream_[sock_ptr] = std::move(st);

    if (live.empty()) {
        // Every dependency resolved inline (missing upstream or open circuit);
        // frame the aggregate without ever touching a backend socket.
        finalize_composite(sock_ptr, client_upstream_[sock_ptr].get());
        return;
    }

    // Drive each live child once: the first advance() may complete synchronously
    // (e.g. an immediate connect failure), and finishing the last child erases
    // the state, so re-check the map on every step.
    for (std::size_t idx : live) {
        auto it = client_upstream_.find(sock_ptr);
        if (it == client_upstream_.end()) return;
        drive_child(sock_ptr, it->second.get(), idx, 0);
    }
}

void HttpHandler::drive_exchange(Socket* client, uint32_t events) {
    auto it = client_upstream_.find(client);
    if (it == client_upstream_.end()) return;
    drive_child(client, it->second.get(), 0, events);
}

void HttpHandler::drive_child(Socket* client, ExchangeState* st, size_t idx, uint32_t events) {
    ChildExchange& ch = *st->children[idx];
    if (!ch.ex) return;

    const UpstreamExchange::Want want = ch.ex->advance(events);

    // Always unregister before re-registering, never mod(): the exchange may
    // have dropped its previous socket (pooled -> fresh retry) and fd() may now
    // be a recycled fd number whose old registration was auto-removed by
    // close(). A stale mod() would then fail with ENOENT, while del()+add() is
    // correct in every case.
    if (ch.fd >= 0) {
        epoll_.del(ch.fd);
        upstream_fd_owner_.erase(ch.fd);
        ch.fd = -1;
    }
    const int upstream_fd = ch.ex->fd();
    if (want != UpstreamExchange::Want::Done && upstream_fd >= 0) {
        // EPOLLONESHOT so a level-triggered upstream fd cannot re-fire while we
        // are still processing this event; the fd is disarmed until the next
        // advance() decides which direction it needs.
        const uint32_t event = want == UpstreamExchange::Want::Read ? EPOLLIN : EPOLLOUT;
        epoll_.add(upstream_fd, event | EPOLLONESHOT);
        upstream_fd_owner_[upstream_fd] = {client, idx};
        ch.fd = upstream_fd;
    }

    if (want == UpstreamExchange::Want::Done) {
        finish_child(client, st, idx);
    }
}

void HttpHandler::finish_child(Socket* client, ExchangeState* st, size_t idx) {
    ChildExchange& ch = *st->children[idx];
    UpstreamExchange* ex = ch.ex.get();
    const BackendResponse& be = ex->result();

    // Record the circuit outcome for every backend call, single or composite, so
    // composite traffic feeds the breaker the same way single-upstream traffic
    // does.
    if (ex->succeeded()) {
        upstream_manager_.record_success(ch.upstream_key, ch.server);
    } else {
        upstream_manager_.record_failure(ch.upstream_key, ch.server,
                                         ch.circuit_failure_threshold, ch.circuit_recovery_timeout_ms);
        Metrics::instance().record_upstream_error(be.error);
    }

    if (st->composite) {
        ch.item["duration_ms"] = composite_elapsed_ms(st->started);
        if (ex->succeeded()) {
            const bool ok = be.status_code >= 200 && be.status_code < 300;
            ch.item["status"] = ok ? "ok" : "failed";
            ch.item["http_status"] = be.status_code;
            ch.item["backend"] = ch.upstream_key;
            ch.item["error"] = ok ? "" : be.body;
            ch.item["timed_out"] = be.status_code == 504;
            if (ok) ch.item["body"] = be.body;
        } else {
            const bool timed_out = be.error == BackendError::ConnectTimeout ||
                                   be.error == BackendError::WriteTimeout ||
                                   be.error == BackendError::ReadTimeout;
            ch.item["status"] = "failed";
            ch.item["http_status"] = timed_out ? 504 : 502;
            ch.item["error"] = timed_out ? "upstream_timeout" : "upstream_unavailable";
            ch.item["timed_out"] = timed_out;
        }
        Logger::get()->info("COMPOSITE dependency={} status={} required={} traceparent={} duration_ms={} http_status={}",
                            ch.name.empty() ? "<unnamed>" : ch.name,
                            ch.item["status"].get<std::string>(), ch.required,
                            ch.item["traceparent"].get<std::string>(),
                            ch.item["duration_ms"].get<std::int64_t>(),
                            ch.item.value("http_status", 0));
        ch.finished = true;
        if (ch.fd >= 0) {
            epoll_.del(ch.fd);
            upstream_fd_owner_.erase(ch.fd);
            ch.fd = -1;
        }
        ch.ex.reset();
        for (const auto& c : st->children) {
            if (!c->finished) return;
        }
        finalize_composite(client, st);
        return;
    }

    if (ex->succeeded()) {
        HttpResponse resp;
        resp.status_code = be.status_code;
        resp.status_message = be.status_code >= 200 && be.status_code < 300 ? "OK" : "Upstream Response";
        resp.body = be.body;
        const std::unordered_set<std::string> conn_tokens = response_connection_tokens(be.headers);
        // One reusable lowercase buffer for all header names of this response;
        // a fresh std::string per header measured hot (alloc + tolower loop).
        std::string lk;
        lk.reserve(48);
        for (const auto& [k, v] : be.headers) {
            // Drop anything carrying a CR/LF in name or value: a bare LF in an
            // upstream header would let a compromised backend split the response
            // or inject headers toward the client.
            if (k.find('\r') != std::string::npos || k.find('\n') != std::string::npos ||
                v.find('\r') != std::string::npos || v.find('\n') != std::string::npos) {
                Logger::get()->warn("Dropping unsafe upstream response header '{}' (contains CR/LF)", k);
                continue;
            }
            lk.clear();
            for (char c : k) lk += ascii_to_lower(static_cast<unsigned char>(c));
            if (is_response_hop_by_hop(lk) || conn_tokens.count(lk) != 0) continue;
            resp.headers[k] = v;
        }
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
        // fd() was unregistered by drive_child() before we got here, so the
        // keep-alive socket may now safely go back to the shared pool.
        ex->reuse_to_pool();
        finalize_response(client, st->req, resp, false);
        client_upstream_.erase(client);
        return;
    }

    const BackendError error = be.error;
    // Route-level retry: a transient failure on an idempotent method may be
    // replayed on a fresh connection up to max_retries times.
    // should_retry_backend_request() additionally caps the total at two tries,
    // mirroring the former retry loop.
    if (ch.attempt_count < ch.max_retries &&
        should_retry_backend_request(st->req.method, error, ch.attempt_count)) {
        ++ch.attempt_count;
        const bool idempotent = ch.params.method == "GET" || ch.params.method == "HEAD" ||
                                ch.params.method == "OPTIONS";
        ch.ex = std::make_unique<UpstreamExchange>(ch.params, idempotent);
        drive_child(client, st, idx, 0);
        return;
    }

    const bool timed_out = error == BackendError::ConnectTimeout ||
                           error == BackendError::WriteTimeout ||
                           error == BackendError::ReadTimeout;
    HttpResponse resp = make_error_response(
        timed_out ? 504 : 502,
        timed_out ? "Gateway Timeout" : "Bad Gateway",
        timed_out ? "The upstream service timed out"
                  : "The upstream service returned an invalid response or could not be reached");
    // finalize_response() reads st->req, so it must run before the state is
    // erased from the map (which destroys it).
    finalize_response(client, st->req, resp, false);
    client_upstream_.erase(client);
}

void HttpHandler::force_finish_child(ExchangeState* st, size_t idx) {
    ChildExchange& ch = *st->children[idx];
    if (ch.finished) return;
    // Unregister and release the exchange: the overall deadline fired while this
    // backend was still in flight. The blocking forward_request this replaces
    // cannot be interrupted mid-syscall, so the deadline's job is to stop the
    // event loop waiting for it, not to abort the socket call.
    if (ch.fd >= 0) {
        epoll_.del(ch.fd);
        upstream_fd_owner_.erase(ch.fd);
        ch.fd = -1;
    }
    ch.ex.reset();
    ch.item["status"] = "failed";
    ch.item["http_status"] = 504;
    ch.item["error"] = "upstream_timeout";
    ch.item["timed_out"] = true;
    ch.item["duration_ms"] = composite_elapsed_ms(st->started);
    ch.finished = true;
    Logger::get()->warn("COMPOSITE dependency={} status={} required={} traceparent={} duration_ms={} reason=total_deadline_exceeded",
                        ch.name.empty() ? "<unnamed>" : ch.name, "failed", ch.required,
                        ch.item["traceparent"].get<std::string>(),
                        ch.item["duration_ms"].get<std::int64_t>());
}

void HttpHandler::finalize_composite(Socket* client, ExchangeState* st) {
    json composite = json::object();
    bool required_failed = false;
    for (const auto& ch : st->children) {
        composite[ch->name] = ch->item;
        const std::string status = ch->item.value("status", "");
        if (ch->required && (status == "failed" || status == "unavailable")) {
            required_failed = true;
        }
    }

    HttpResponse resp;
    resp.status_code = required_failed ? 503 : 200;
    resp.status_message = required_failed ? "Service Unavailable" : "OK";
    resp.headers["Content-Type"] = "application/json";
    json payload = json::object();
    // `route` identifies the matched route (same value used by metrics and the
    // audit log); `aggregate` identifies the composition itself, giving the
    // otherwise-unused composite_target.name a real runtime effect.
    payload["route"] = st->route_name;
    payload["aggregate"] = st->composite_name;
    payload["status"] = required_failed ? "partial_failure" : "ok";
    payload["dependencies"] = composite;
    resp.body = payload.dump();
    resp.headers["Content-Length"] = std::to_string(resp.body.size());
    // finalize_response() reads st->req, so it must run before the state is
    // erased from the map (which destroys it).
    finalize_response(client, st->req, resp, false);
    client_upstream_.erase(client);
}

void HttpHandler::abort_upstream_for_client(Socket* client) {
    auto it = client_upstream_.find(client);
    if (it == client_upstream_.end()) return;
    ExchangeState* st = it->second.get();
    for (const auto& ch : st->children) {
        if (ch->fd >= 0) {
            epoll_.del(ch->fd);
            upstream_fd_owner_.erase(ch->fd);
        }
    }
    // Dropping the state destroys every upstream socket (closing it, or removing
    // it from a pool checkout) and frees the snapshotted request. The client
    // socket is still held by `st->client` until this erase completes.
    client_upstream_.erase(it);
}

bool HttpHandler::is_upstream_fd(int fd) const {
    return upstream_fd_owner_.count(fd) != 0;
}

void HttpHandler::on_upstream_event(int fd, uint32_t events) {
    const auto it = upstream_fd_owner_.find(fd);
    if (it == upstream_fd_owner_.end()) return;
    Socket* client = it->second.first;
    const size_t idx = it->second.second;
    auto st_it = client_upstream_.find(client);
    if (st_it == client_upstream_.end()) return;
    drive_child(client, st_it->second.get(), idx, events);
}

int HttpHandler::poll_timeout_ms() const {
    if (client_upstream_.empty()) return 1000;
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();
    int best = 1000;
    auto consider = [&best, now](const clock::time_point& deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int ms = remaining < 0 ? 0 : static_cast<int>(remaining);
        if (ms < best) best = ms;
    };
    for (const auto& [client, st] : client_upstream_) {
        // The composite's hard overall deadline is a first-class timer, separate
        // from each child's idempotent-idle deadline.
        if (st->composite && st->has_total_deadline) consider(st->total_deadline);
        for (const auto& ch : st->children) {
            if (ch->ex && !ch->finished) consider(ch->ex->deadline());
        }
    }
    return best;
}

void HttpHandler::check_upstream_timeouts() {
    if (client_upstream_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    // Snapshot the affected clients first: driving an exchange can complete (and
    // therefore erase) it, which would invalidate an iterator over the map.
    std::vector<Socket*> due;
    due.reserve(client_upstream_.size());
    for (const auto& [client, st] : client_upstream_) {
        bool expired = st->composite && st->has_total_deadline && st->total_deadline <= now;
        if (!expired) {
            for (const auto& ch : st->children) {
                if (ch->ex && !ch->finished && ch->ex->deadline() <= now) { expired = true; break; }
            }
        }
        if (expired) due.push_back(client);
    }
    for (Socket* client : due) {
        auto it = client_upstream_.find(client);
        if (it == client_upstream_.end()) continue;

        // Overall deadline: a hard wall clock. Because UpstreamExchange refreshes
        // a child's idle deadline on progress, a slowly-streaming backend could
        // otherwise outlive the aggregate budget forever, so abandon every child
        // still in flight and finalize.
        ExchangeState* st = it->second.get();
        if (st->composite && st->has_total_deadline && st->total_deadline <= now) {
            for (std::size_t i = 0; i < st->children.size(); ++i) {
                force_finish_child(st, i);
            }
            finalize_composite(client, st);
            continue;
        }

        // A child hit its own idle deadline: driving it with no events makes
        // advance() surface the timeout. Each step may finish the last child and
        // erase the state, so re-check the map.
        std::size_t n = st->children.size();
        for (std::size_t i = 0; i < n; ++i) {
            auto it2 = client_upstream_.find(client);
            if (it2 == client_upstream_.end()) break;
            ExchangeState* s = it2->second.get();
            if (i >= s->children.size()) break;
            ChildExchange& ch = *s->children[i];
            if (ch.ex && !ch.finished && ch.ex->deadline() <= now) {
                drive_child(client, s, i, 0);
            }
        }
    }
}

void HttpHandler::rearm_reads(Socket* sock) {
    // While an upstream round trip is in flight, leave the fd disarmed: EPOLLONESHOT
    // has already fired, and re-arming here would let a pipelined second request be
    // parsed and dispatched while the first is still outstanding, breaking
    // request/response ordering. The child completion path re-arms once the response is framed.
    if (client_upstream_.count(sock)) return;
    const int fd = sock->getFd();
    if (!send_queues_[sock].empty()) {
        epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
    } else {
        epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
    }
}

void HttpHandler::send_error_response(Socket* sock, int code, const std::string& message,const std::string& allow_methods){
    HttpResponse resp = make_error_response(code, message, message);
    auto req_it = last_requests_.find(sock);
    if (req_it != last_requests_.end() && !req_it->second.trace_id.empty()) {
        resp.headers["X-Trace-Id"] = req_it->second.trace_id;
    }
    if (req_it != last_requests_.end() && req_it->second.headers.count("traceparent")) {
        resp.headers["Traceparent"] = req_it->second.headers.at("traceparent");
    }
    keep_alive_[sock] = false;
    resp.headers["Connection"] = "close";
    if (!allow_methods.empty()) {
        resp.headers["Allow"] = allow_methods;
    }

    std::string header = headers_to_string(resp);
    // Error bodies are small inline HTML; coalesce and move into the queue (one
    // chunk, one write) with no extra copy.
    header.append(resp.body);
    const size_t total_resp_bytes = header.size();
    send_queues_[sock].emplace_back(std::move(header));

    resp_status_[sock] = code;
    resp_size_[sock] = total_resp_bytes;

    int fd = sock->getFd();
    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
}

std::string HttpHandler::headers_to_string(const HttpResponse& resp) {
    // Direct string building with one reserve; ostringstream measured hot on
    // the per-response serialization path.
    size_t est = 32 + resp.status_message.size() + 4;
    for (const auto& [key, value] : resp.headers) {
        est += key.size() + value.size() + 4;
    }
    std::string out;
    out.reserve(est);
    out += "HTTP/1.1 ";
    out += std::to_string(resp.status_code);
    out += ' ';
    out += resp.status_message;
    out += "\r\n";
    for (const auto& [key, value] : resp.headers) {
        out += key;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "\r\n";   // Blank line separating headers and body
    return out;
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
    ssl_file_buffers_.erase(sock_ptr);
    pending_ssl_writes_.erase(sock_ptr);
    client_ip_map_.erase(sock_ptr);
    request_start_time_.erase(sock_ptr);
    last_requests_.erase(sock_ptr);
    // Drop any in-flight upstream round trip for this connection: unregister its
    // upstream fd and free the snapshotted state. The exchange keeps the client
    // Socket alive via its own shared_ptr, so it is closed last.
    abort_upstream_for_client(sock_ptr);
    Logger::get()->info("HttpHandler: connection closed on fd {}", fd);
}

void HttpHandler::wait_for_ssl_write(Socket* sock, SSLWant want) {
    pending_ssl_writes_[sock] = want;
    const uint32_t event = want == SSLWant::READ ? EPOLLIN : EPOLLOUT;
    epoll_.mod(sock->getFd(), event | EPOLLET | EPOLLONESHOT);
}

bool HttpHandler::has_pending_ssl_write(Socket* sock) const {
    return pending_ssl_writes_.find(sock) != pending_ssl_writes_.end();
}

void HttpHandler::handle_write(std::shared_ptr<Socket> sock){
    int fd = sock->getFd();
    Socket* sock_ptr = sock.get();
    try {
        // Keep processing until there is no data to send and no new request to handle
        while (true) {
            auto& queue = send_queues_[sock_ptr];

            // ==================== Phase 1: send data from the user-space queue ====================
            // Drop empty chunks so a gather always has payload to write.
            while (!queue.empty() && queue.front().empty()) queue.pop_front();
            const bool ssl_conn = sock_ptr->get_is_ssl_();
            while (!queue.empty()) {
                if (!ssl_conn) {
                    // Plaintext: gather the queued chunks into iovecs and issue one
                    // writev for the whole response (nginx-style gather) instead of
                    // one send() syscall per chunk.
                    constexpr int kMaxIov = 64;
                    struct iovec iov[kMaxIov];
                    int nio = 0;
                    for (auto& chunk : queue) {
                        if (nio == kMaxIov) break;
                        iov[nio].iov_base = chunk.data();
                        iov[nio].iov_len  = chunk.size();
                        ++nio;
                    }
                    ssize_t n = ::writev(fd, iov, nio);
                    if (n > 0) {
                        size_t left = static_cast<size_t>(n);
                        while (left > 0 && !queue.empty()) {
                            auto& front = queue.front();
                            if (left >= front.size()) {
                                left -= front.size();
                                queue.pop_front();
                            } else {
                                front.erase(front.begin(), front.begin() + static_cast<ptrdiff_t>(left));
                                left = 0;
                            }
                        }
                    } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                        break;
                    } else if (n == -1 && errno == EPIPE) {
                        Logger::get()->debug("Client disconnected (EPIPE) on fd {}", fd);
                        cleanup(sock);
                        return;
                    } else {
                        throw_system_error("writev");
                    }
                    continue;
                }

                auto& front = queue.front();
                int n = sock->sslWrite(front.data(), front.size());
                if (n > 0) {
                    pending_ssl_writes_.erase(sock_ptr);
                    if (n == front.size()) {
                        queue.pop_front();
                    } else {
                        front.erase(front.begin(), front.begin() + n);
                        // Not fully sent, wait for the next EPOLLOUT
                        epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                        break;  // Kernel buffer full
                    }
                } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    wait_for_ssl_write(sock_ptr, sock->last_ssl_want());
                    return;
                } else if (n == -1 && errno == EPIPE) {
                    // The client has disconnected; this is normal, just clean up the connection
                    Logger::get()->debug("Client disconnected (EPIPE) on fd {}", fd);
                    cleanup(sock);
                    return;
                } else {
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
                        // Keep each user-space chunk alive across WANT_READ/WANT_WRITE retries.
                        constexpr size_t SSL_SENDFILE_BUF = 65536;  // 64KB buffer
                        auto& filebuf = ssl_file_buffers_[sock_ptr];
                        while (remaining > 0) {
                            if (filebuf.empty()) {
                                filebuf.resize(std::min(static_cast<off_t>(SSL_SENDFILE_BUF), remaining));
                                const ssize_t read_n = ::pread(file_fd, filebuf.data(), filebuf.size(), offset);
                                if (read_n <= 0) {
                                    Logger::get()->error("pread for SSL file failed or file was truncated: {}",
                                                         read_n == -1 ? strerror(errno) : "unexpected EOF");
                                    file_fds_.erase(sock_ptr);
                                    file_offsets_.erase(sock_ptr);
                                    file_sizes_.erase(sock_ptr);
                                    ssl_file_buffers_.erase(sock_ptr);
                                    cleanup(sock);
                                    return;
                                }
                                filebuf.resize(static_cast<size_t>(read_n));
                            }

                            const int write_n = sock->sslWrite(filebuf.data(), filebuf.size());
                            if (write_n > 0) {
                                pending_ssl_writes_.erase(sock_ptr);
                                offset += write_n;
                                remaining -= write_n;
                                filebuf.erase(filebuf.begin(), filebuf.begin() + write_n);
                                file_offsets_[sock_ptr] = offset;
                                if (!filebuf.empty()) {
                                    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                                    return;
                                }
                            } else if (write_n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                file_offsets_[sock_ptr] = offset;
                                wait_for_ssl_write(sock_ptr, sock->last_ssl_want());
                                return;
                            } else {
                                Logger::get()->error("SSL_write for file failed on fd {}", fd);
                                file_fds_.erase(sock_ptr);
                                file_offsets_.erase(sock_ptr);
                                file_sizes_.erase(sock_ptr);
                                ssl_file_buffers_.erase(sock_ptr);
                                cleanup(sock);
                                return;
                            }
                        }
                        ssl_file_buffers_.erase(sock_ptr);
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
                                   audit_request.tenant.empty() ? "-" : audit_request.tenant,
                                   config_.logging);
                }

                // Access log is independently configurable; audit remains on.
                if (config_.logging.access_log_enabled) {
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
                        path = logging_utils::redact_request_target(
                            req_it->second.path, req_it->second.query, config_.logging);
                        version = req_it->second.version;
                        auto ua = req_it->second.headers.find("user-agent");
                        user_agent = (ua != req_it->second.headers.end())
                            ? logging_utils::redact_header_value("user-agent", ua->second, config_.logging)
                            : "-";
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
                } else {
                    auto req_it = last_requests_.find(sock_ptr);
                    if (req_it != last_requests_.end()) {
                        last_requests_.erase(sock_ptr);
                    }
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
                    // A pipelined request may have been handed to an upstream
                    // exchange; the fd is intentionally left disarmed until
                    // the child completion path delivers its response.
                    if (client_upstream_.count(sock_ptr)) {
                        return;
                    }
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

bool HttpHandler::has_inflight_work(Socket* sock) const {
    if (sock == nullptr) return false;
    // An in-flight upstream round trip keeps this connection busy even though its
    // send queue is still empty; the graceful-shutdown drain must wait for it
    // rather than close the connection mid-flight.
    if (client_upstream_.count(sock)) return true;
    const auto rb = read_bufs_.find(sock);
    if (rb != read_bufs_.end() && !rb->second.empty()) return true;
    const auto rr = request_ready_.find(sock);
    if (rr != request_ready_.end() && rr->second) return true;
    const auto sq = send_queues_.find(sock);
    if (sq != send_queues_.end() && !sq->second.empty()) return true;
    if (file_fds_.count(sock)) return true;   // sendfile in progress
    return false;
}

bool HttpHandler::requires_auth(const HttpRequest& req) const {
    // Only /api/-prefixed endpoints require auth; static files and public routes like /metrics need none
    return req.path.rfind("/api/", 0) == 0;
}

bool HttpHandler::should_rate_limit(const HttpRequest& req, const GatewayRoute& route, const std::string& client_ip, const ApiKeyConfig* api_key_cfg) const {
    // Operations/probe endpoints are exempt: a load balancer or Prometheus
    // scraping frequently must not trip the per-IP limiter and break probing.
    static const char* kExemptPaths[] = {"/healthz", "/readyz", "/version", "/metrics"};
    for (const char* p : kExemptPaths) {
        if (req.path == p) return false;
    }
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