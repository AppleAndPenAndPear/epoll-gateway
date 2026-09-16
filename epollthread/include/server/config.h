#pragma once
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

struct RateLimitConfig {
    size_t capacity = 100;
    size_t refill_per_second = 50;
};

struct ApiKeyConfig {
    std::string key;
    std::string name;       // Display name of the API key describing its purpose
    RateLimitConfig rate_limit{200, 100};   // Per-key rate limit quota
    std::vector<std::string> allowed_hosts;
    std::vector<std::string> allowed_tenants;
};

// Upstream server
struct UpstreamServer {
    std::string host;
    int port;
    bool healthy = true;          // Whether currently healthy
    int consecutive_failures = 0; // Consecutive failure count
};

// A group of upstream servers (one backend service)
struct Upstream {
    std::vector<UpstreamServer> servers;
    std::string algorithm;  // Load balancing algorithm
};

// Describes which upstream a route uses, plus the route's execution parameters
struct UpstreamTarget {
    std::string name;
    int timeout_ms = 5000;      // Max wait time when requesting the backend
    int max_retries = 1;        // Extra retries allowed per request
    int circuit_failure_threshold = 5;
    // Minimum wait after the circuit opens before allowing one half-open probe request.
    int circuit_recovery_timeout_ms = 10000;
};

// Gateway route rule: matches the path and carries security policy and execution target
struct GatewayRoute {
    std::string name;
    std::string method = "GET";
    std::string path = "/";
    std::string host = "*";               // Allowed Host, e.g. "api.example.com"
    std::string tenant = "*";             // Allowed tenant, e.g. "team-a"
    std::string target_type = "upstream";  // local | upstream | static
    std::string handler_name;
    UpstreamTarget upstream_target;
    std::string static_root;

    bool enabled = true;        // Whether enabled
    bool auth_required = true;
    bool allow_anonymous = false;

    std::string rate_limit_policy = "api_key"; // api_key | ip | route

    std::vector<std::string> allowed_api_keys;
    std::vector<std::string> denied_api_keys;
};

struct UpstreamConfig {
    std::unordered_map<std::string, Upstream> upstreams;
    std::vector<GatewayRoute> routes;
    int health_check_timeout_ms = 500;
};

struct Config {
    // Server port
    unsigned short port = 5005;
    // listen backlog
    int backlog = 1024;
    // Number of worker threads (0 means use hardware_concurrency)
    unsigned int num_workers = 0;
    // Static file root directory
    std::string www_root = "./www";

    size_t cache_max_entries = 1024;  // Max entries in the per-thread cache
    size_t cache_max_file_size_mb = 1; // Max cacheable file size (MB)

    int keepalive_timeout = 60; // Keep-alive timeout (seconds)
    // Dynamic thread pool configuration
    struct ThreadPoolConfig {
        size_t min_threads = 2;
        size_t max_threads = 10;
        size_t scale_up_threshold = 2;
        size_t scale_down_threshold = 1;
    } thread_pool;

    UpstreamConfig upstream_config;

    RateLimitConfig rate_limit_config;

    std::vector<ApiKeyConfig> api_keys;      // API key list

    static bool is_valid_file(const std::string& path) {
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            return false;
        }
        try {
            nlohmann::json j;
            ifs >> j;
            return j.is_object();
        } catch (...) {
            return false;
        }
    }

    // Load config from a JSON file; keeps defaults if the file is missing or parsing fails
    static Config from_file(const std::string& path) {
        Config config;
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            // File missing; use defaults
            return config;
        }
        nlohmann::json j;
        try {
            ifs >> j;
        } catch (...) {
            // Parse failed; use defaults
            return config;
        }
        // Read field by field, overriding defaults when present
        if (j.contains("port")) config.port = j["port"];
        if (j.contains("backlog")) config.backlog = j["backlog"];
        if (j.contains("num_workers")) config.num_workers = j["num_workers"];
        if (j.contains("www_root")) config.www_root = j["www_root"];

        if (j.contains("thread_pool")) {
            auto& tp = j["thread_pool"];
            if (tp.contains("min")) config.thread_pool.min_threads = tp["min"];
            if (tp.contains("max")) config.thread_pool.max_threads = tp["max"];
            if (tp.contains("scale_up")) config.thread_pool.scale_up_threshold = tp["scale_up"];
            if (tp.contains("scale_down")) config.thread_pool.scale_down_threshold = tp["scale_down"];
        }
        if (j.contains("cache_max_entries")) {
            config.cache_max_entries = j["cache_max_entries"];
        }
        if (j.contains("cache_max_file_size_mb")) {
            config.cache_max_file_size_mb = j["cache_max_file_size_mb"];
        }
        if (j.contains("keepalive_timeout")) {
            config.keepalive_timeout = j["keepalive_timeout"];
        }

        // Parse upstreams
        if (j.contains("upstreams")) {
            for (auto& [name, val] : j["upstreams"].items()) {
                Upstream up;
                if (val.contains("servers")) {
                    for (auto& srv : val["servers"]) {
                        UpstreamServer s;
                        s.host = srv.value("host", "127.0.0.1");
                        s.port = srv.value("port", 80);
                        up.servers.push_back(s);
                    }
                }
                up.algorithm = val.value("algorithm", "round_robin");
                config.upstream_config.upstreams[name] = up;
            }
        }
        if (j.contains("upstream_health_check_timeout_ms")) {
            config.upstream_config.health_check_timeout_ms =
                j.value("upstream_health_check_timeout_ms", 500);
        }

        // Parse routes
        if (j.contains("routes")) {
            for (auto& item : j["routes"]) {
                GatewayRoute r;
                r.name = item.value("name", "");
                r.method = item.value("method", "GET");
                r.path = item.value("path", "/");
                r.host = item.value("host", "*");
                r.tenant = item.value("tenant", "*");
                r.target_type = item.value("target_type", "upstream");
                r.handler_name = item.value("handler", "");
                r.static_root = item.value("static_root", "");

                r.enabled = item.value("enabled", true);
                r.auth_required = item.value("auth_required", true);
                r.allow_anonymous = item.value("allow_anonymous", false);
                r.rate_limit_policy = item.value("rate_limit_policy", "api_key");
                const int legacy_timeout_ms = item.value("timeout_ms", 5000);

                if (item.contains("upstream_target")) {
                    const auto& target = item["upstream_target"];
                    r.upstream_target.name = target.value("name", "");
                    r.upstream_target.timeout_ms = target.value("timeout_ms", legacy_timeout_ms);
                    r.upstream_target.max_retries = target.value("max_retries", 1);
                    r.upstream_target.circuit_failure_threshold = target.value("circuit_failure_threshold", 5);
                    r.upstream_target.circuit_recovery_timeout_ms = target.value("circuit_recovery_timeout_ms", 10000);
                } else {
                    r.upstream_target.name = "";
                    r.upstream_target.timeout_ms = legacy_timeout_ms;
                    r.upstream_target.max_retries = 1;
                    r.upstream_target.circuit_failure_threshold = 5;
                    r.upstream_target.circuit_recovery_timeout_ms = 10000;
                }

                if (item.contains("allowed_api_keys")) {
                    for (const auto& k : item["allowed_api_keys"]) {
                        r.allowed_api_keys.push_back(k.get<std::string>());
                    }
                }
                if (item.contains("denied_api_keys")) {
                    for (const auto& k : item["denied_api_keys"]) {
                        r.denied_api_keys.push_back(k.get<std::string>());
                    }
                }

                config.upstream_config.routes.push_back(r);
            }
        }

        // Parse rate limit config
        if (j.contains("rate_limit")){
            auto& rl = j["rate_limit"];
            if (rl.contains("capacity")) {
                config.rate_limit_config.capacity = rl["capacity"];
            }
            if (rl.contains("refill_per_second")) {
                config.rate_limit_config.refill_per_second = rl["refill_per_second"];
            }
        }

        if(j.contains("api_keys")){
            for (auto& item : j["api_keys"]) {
                ApiKeyConfig ak;
                ak.key = item.value("key", "");
                ak.name = item.value("name", "");
                if (item.contains("rate_limit")) {
                    auto& rl = item["rate_limit"];
                    ak.rate_limit.capacity = rl.value("capacity", 200);
                    ak.rate_limit.refill_per_second = rl.value("refill_per_second", 100);
                }
                if (item.contains("allowed_hosts")) {
                    for (const auto& host : item["allowed_hosts"]) {
                        ak.allowed_hosts.push_back(host.get<std::string>());
                    }
                }
                if (item.contains("allowed_tenants")) {
                    for (const auto& tenant : item["allowed_tenants"]) {
                        ak.allowed_tenants.push_back(tenant.get<std::string>());
                    }
                }
                config.api_keys.push_back(ak);
            }
        }

        return config;
    }
};