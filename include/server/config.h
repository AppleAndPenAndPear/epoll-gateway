#pragma once
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <set>
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
    std::string algorithm = "round_robin";  // Load balancing algorithm
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

// TLS server material and hardening knobs
struct TlsConfig {
    std::string cert_path = "certs/server.crt";
    std::string key_path = "certs/server.key";
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

    TlsConfig tls;

    RateLimitConfig rate_limit_config;

    std::vector<ApiKeyConfig> api_keys;      // API key list
    // Path to a separate api-keys JSON file; empty when keys were provided
    // inline or via GW_API_KEYS ("<env:GW_API_KEYS>")
    std::string api_keys_file;

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

    // Semantic validation of a fully-parsed configuration. Returns human-readable
    // problems; an empty list means the config is safe to apply.
    static std::vector<std::string> validate(const Config& config) {
        std::vector<std::string> errors;
        if (config.port == 0) errors.push_back("port: must be in [1, 65535]");
        if (config.backlog <= 0) errors.push_back("backlog: must be > 0");
        if (config.keepalive_timeout <= 0) errors.push_back("keepalive_timeout: must be > 0");
        if (config.cache_max_entries == 0) errors.push_back("cache_max_entries: must be > 0");
        if (config.cache_max_file_size_mb == 0) errors.push_back("cache_max_file_size_mb: must be > 0");
        if (config.thread_pool.min_threads == 0) errors.push_back("thread_pool.min: must be > 0");
        if (config.thread_pool.max_threads < config.thread_pool.min_threads)
            errors.push_back("thread_pool.max: must be >= thread_pool.min");
        if (config.thread_pool.scale_up_threshold == 0) errors.push_back("thread_pool.scale_up: must be > 0");
        if (config.thread_pool.scale_down_threshold == 0) errors.push_back("thread_pool.scale_down: must be > 0");
        if (config.rate_limit_config.capacity == 0) errors.push_back("rate_limit.capacity: must be > 0");
        if (config.rate_limit_config.refill_per_second == 0)
            errors.push_back("rate_limit.refill_per_second: must be > 0");
        if (config.tls.cert_path.empty())
            errors.push_back("tls.cert_path: must not be empty");
        if (config.tls.key_path.empty())
            errors.push_back("tls.key_path: must not be empty");

        for (const auto& [name, up] : config.upstream_config.upstreams) {
            if (up.servers.empty())
                errors.push_back("upstreams." + name + ": at least one server is required");
            if (up.algorithm.empty())
                errors.push_back("upstreams." + name + ".algorithm: must not be empty");
            for (size_t i = 0; i < up.servers.size(); ++i) {
                const auto& s = up.servers[i];
                const std::string at = "upstreams." + name + ".servers[" + std::to_string(i) + "]";
                if (s.host.empty()) errors.push_back(at + ".host: must not be empty");
                if (s.port <= 0 || s.port > 65535)
                    errors.push_back(at + ".port: must be in [1, 65535]");
            }
        }

        std::set<std::string> route_names;
        std::set<std::string> match_keys;
        for (size_t i = 0; i < config.upstream_config.routes.size(); ++i) {
            const auto& r = config.upstream_config.routes[i];
            const std::string at = "routes[" + std::to_string(i) + "]" +
                                   (r.name.empty() ? "" : " (" + r.name + ")");
            if (r.name.empty()) {
                errors.push_back(at + ".name: must not be empty");
            } else if (!route_names.insert(r.name).second) {
                errors.push_back(at + ".name: duplicate route name '" + r.name + "'");
            }
            if (r.method.empty()) errors.push_back(at + ".method: must not be empty");
            if (r.path.empty() || r.path[0] != '/')
                errors.push_back(at + ".path: must start with '/'");
            if (r.target_type != "local" && r.target_type != "upstream" && r.target_type != "static") {
                errors.push_back(at + ".target_type: unknown value '" + r.target_type +
                                 "' (expected local|upstream|static)");
            }
            if (r.rate_limit_policy != "api_key" && r.rate_limit_policy != "ip" &&
                r.rate_limit_policy != "route") {
                errors.push_back(at + ".rate_limit_policy: unknown value '" + r.rate_limit_policy +
                                 "' (expected api_key|ip|route)");
            }
            if (r.target_type == "upstream") {
                if (r.upstream_target.name.empty()) {
                    errors.push_back(at + ".upstream_target.name: required for target_type=upstream");
                } else if (config.upstream_config.upstreams.find(r.upstream_target.name) ==
                           config.upstream_config.upstreams.end()) {
                    errors.push_back(at + ".upstream_target.name: references unknown upstream '" +
                                     r.upstream_target.name + "'");
                }
            } else if (r.target_type == "static" && r.static_root.empty()) {
                errors.push_back(at + ".static_root: required for target_type=static");
            }
            if (r.upstream_target.timeout_ms <= 0)
                errors.push_back(at + ".upstream_target.timeout_ms: must be > 0");
            if (r.upstream_target.max_retries < 0)
                errors.push_back(at + ".upstream_target.max_retries: must be >= 0");
            if (r.upstream_target.circuit_failure_threshold <= 0)
                errors.push_back(at + ".upstream_target.circuit_failure_threshold: must be > 0");
            if (r.upstream_target.circuit_recovery_timeout_ms <= 0)
                errors.push_back(at + ".upstream_target.circuit_recovery_timeout_ms: must be > 0");

            const std::string match_key = r.method + "|" + r.host + "|" + r.tenant + "|" + r.path;
            if (!match_keys.insert(match_key).second)
                errors.push_back(at + ": duplicate route match (method/host/tenant/path): " + match_key);
        }

        std::set<std::string> seen_keys;
        for (size_t i = 0; i < config.api_keys.size(); ++i) {
            const auto& ak = config.api_keys[i];
            const std::string at = "api_keys[" + std::to_string(i) + "]";
            if (ak.key.empty()) {
                errors.push_back(at + ".key: must not be empty");
            } else if (!seen_keys.insert(ak.key).second) {
                errors.push_back(at + ".key: duplicate api key");
            }
            if (ak.rate_limit.capacity == 0)
                errors.push_back(at + ".rate_limit.capacity: must be > 0");
            if (ak.rate_limit.refill_per_second == 0)
                errors.push_back(at + ".rate_limit.refill_per_second: must be > 0");
        }
        return errors;
    }

    // Load config from a JSON file; keeps defaults if the file is missing or parsing fails.
    // When `errors` is provided it collects schema violations (wrong types, out-of-range
    // values): the offending field keeps its default so the result is still a usable
    // Config, and callers should reject it via validate() before applying.
    static Config from_file(const std::string& path, std::vector<std::string>* errors = nullptr) {
        Config config;
        std::vector<std::string> local_errors;
        std::vector<std::string>& errs = errors ? *errors : local_errors;
        errs.clear();

        // Reads an integer field with type and range checks. Returns false (and
        // records an error, leaving `out` untouched) when present but invalid.
        auto read_int = [&errs](const nlohmann::json& obj, const char* key,
                                long long min_value, long long max_value,
                                long long& out, const std::string& context) {
            const auto it = obj.find(key);
            if (it == obj.end()) return true;
            if (!it->is_number_integer()) {
                errs.push_back(context + key + ": expected an integer");
                return false;
            }
            long long value = 0;
            try {
                value = it->get<long long>();
            } catch (...) {
                errs.push_back(context + key + ": integer out of range");
                return false;
            }
            if (value < min_value || value > max_value) {
                errs.push_back(context + key + ": value " + std::to_string(value) +
                               " out of range [" + std::to_string(min_value) + ", " +
                               std::to_string(max_value) + "]");
                return false;
            }
            out = value;
            return true;
        };
        // Reads a string field with a type check.
        auto read_str = [&errs](const nlohmann::json& obj, const char* key,
                                std::string& out, const std::string& context) {
            const auto it = obj.find(key);
            if (it == obj.end()) return true;
            if (!it->is_string()) {
                errs.push_back(context + key + ": expected a string");
                return false;
            }
            out = it->get<std::string>();
            return true;
        };
        // Reads a boolean field with a type check.
        auto read_bool = [&errs](const nlohmann::json& obj, const char* key,
                                 bool& out, const std::string& context) {
            const auto it = obj.find(key);
            if (it == obj.end()) return true;
            if (!it->is_boolean()) {
                errs.push_back(context + key + ": expected a boolean");
                return false;
            }
            out = it->get<bool>();
            return true;
        };
        // Parses an api-keys JSON array shared by all api key sources.
        auto parse_api_keys_array = [&errs, &read_str, &read_int](
                const nlohmann::json& arr, const char* source,
                std::vector<ApiKeyConfig>& out) {
            if (!arr.is_array()) {
                errs.push_back(std::string(source) + ": expected an array");
                return;
            }
            int index = 0;
            for (auto& item : arr) {
                const std::string at = std::string(source) + "[" + std::to_string(index++) + "].";
                ApiKeyConfig ak;
                read_str(item, "key", ak.key, at);
                read_str(item, "name", ak.name, at);
                if (item.contains("rate_limit")) {
                    auto& rl = item["rate_limit"];
                    long long v = ak.rate_limit.capacity;
                    if (read_int(rl, "capacity", 1, 1000000000, v, at + "rate_limit."))
                        ak.rate_limit.capacity = static_cast<size_t>(v);
                    v = ak.rate_limit.refill_per_second;
                    if (read_int(rl, "refill_per_second", 1, 1000000000, v, at + "rate_limit."))
                        ak.rate_limit.refill_per_second = static_cast<size_t>(v);
                }
                if (item.contains("allowed_hosts")) {
                    if (!item["allowed_hosts"].is_array()) {
                        errs.push_back(at + "allowed_hosts: expected an array");
                    } else {
                        for (const auto& host : item["allowed_hosts"]) {
                            if (host.is_string()) ak.allowed_hosts.push_back(host.get<std::string>());
                            else errs.push_back(at + "allowed_hosts: entries must be strings");
                        }
                    }
                }
                if (item.contains("allowed_tenants")) {
                    if (!item["allowed_tenants"].is_array()) {
                        errs.push_back(at + "allowed_tenants: expected an array");
                    } else {
                        for (const auto& tenant : item["allowed_tenants"]) {
                            if (tenant.is_string()) ak.allowed_tenants.push_back(tenant.get<std::string>());
                            else errs.push_back(at + "allowed_tenants: entries must be strings");
                        }
                    }
                }
                out.push_back(ak);
            }
        };
        // Applies the GW_API_KEYS environment variable (highest-priority api key
        // source); defined before any early return so it always runs.
        auto apply_env_keys = [&errs, &parse_api_keys_array](Config& cfg) {
            if (const char* env_keys = std::getenv("GW_API_KEYS"); env_keys && *env_keys) {
                nlohmann::json ej;
                try {
                    ej = nlohmann::json::parse(env_keys);
                } catch (...) {
                    errs.push_back("GW_API_KEYS: invalid JSON");
                }
                if (!ej.is_null()) {
                    cfg.api_keys.clear();
                    cfg.api_keys_file = "<env:GW_API_KEYS>";
                    parse_api_keys_array(ej, "GW_API_KEYS", cfg.api_keys);
                }
            }
        };

        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            // File missing; use defaults (GW_API_KEYS still applies)
            apply_env_keys(config);
            return config;
        }
        nlohmann::json j;
        try {
            ifs >> j;
        } catch (const std::exception& e) {
            errs.push_back(std::string("invalid JSON: ") + e.what());
            return config;
        }
        if (!j.is_object()) {
            errs.push_back("top-level value must be a JSON object");
            return config;
        }

        // Read field by field, overriding defaults when present
        {
            long long v = config.port;
            if (read_int(j, "port", 1, 65535, v, "")) config.port = static_cast<unsigned short>(v);
        }
        {
            long long v = config.backlog;
            if (read_int(j, "backlog", 1, 65535, v, "")) config.backlog = static_cast<int>(v);
        }
        {
            long long v = config.num_workers;
            if (read_int(j, "num_workers", 0, 1024, v, "")) config.num_workers = static_cast<unsigned int>(v);
        }
        read_str(j, "www_root", config.www_root, "");

        if (j.contains("thread_pool")) {
            auto& tp = j["thread_pool"];
            long long v = config.thread_pool.min_threads;
            if (read_int(tp, "min", 1, 1000000, v, "thread_pool."))
                config.thread_pool.min_threads = static_cast<size_t>(v);
            v = config.thread_pool.max_threads;
            if (read_int(tp, "max", 1, 1000000, v, "thread_pool."))
                config.thread_pool.max_threads = static_cast<size_t>(v);
            v = config.thread_pool.scale_up_threshold;
            if (read_int(tp, "scale_up", 1, 1000000, v, "thread_pool."))
                config.thread_pool.scale_up_threshold = static_cast<size_t>(v);
            v = config.thread_pool.scale_down_threshold;
            if (read_int(tp, "scale_down", 1, 1000000, v, "thread_pool."))
                config.thread_pool.scale_down_threshold = static_cast<size_t>(v);
        }
        {
            long long v = config.cache_max_entries;
            if (read_int(j, "cache_max_entries", 1, 1000000000, v, ""))
                config.cache_max_entries = static_cast<size_t>(v);
        }
        {
            long long v = config.cache_max_file_size_mb;
            if (read_int(j, "cache_max_file_size_mb", 1, 1000000, v, ""))
                config.cache_max_file_size_mb = static_cast<size_t>(v);
        }
        {
            long long v = config.keepalive_timeout;
            if (read_int(j, "keepalive_timeout", 1, 86400, v, ""))
                config.keepalive_timeout = static_cast<int>(v);
        }

        // Parse upstreams
        if (j.contains("upstreams")) {
            if (!j["upstreams"].is_object()) {
                errs.push_back("upstreams: expected an object");
            } else {
                for (auto& [name, val] : j["upstreams"].items()) {
                    Upstream up;
                    if (val.contains("servers")) {
                        if (!val["servers"].is_array()) {
                            errs.push_back("upstreams." + name + ".servers: expected an array");
                        } else {
                            int index = 0;
                            for (auto& srv : val["servers"]) {
                                const std::string ctx = "upstreams." + name +
                                                        ".servers[" + std::to_string(index++) + "].";
                                UpstreamServer s;
                                read_str(srv, "host", s.host, ctx);
                                long long p = 80;
                                if (read_int(srv, "port", 1, 65535, p, ctx)) s.port = static_cast<int>(p);
                                up.servers.push_back(s);
                            }
                        }
                    }
                    read_str(val, "algorithm", up.algorithm, "upstreams." + name + ".");
                    config.upstream_config.upstreams[name] = up;
                }
            }
        }
        {
            long long v = config.upstream_config.health_check_timeout_ms;
            if (read_int(j, "upstream_health_check_timeout_ms", 1, 600000, v, ""))
                config.upstream_config.health_check_timeout_ms = static_cast<int>(v);
        }

        // Parse routes
        if (j.contains("routes")) {
            if (!j["routes"].is_array()) {
                errs.push_back("routes: expected an array");
            } else {
                for (auto& item : j["routes"]) {
                    GatewayRoute r;
                    read_str(item, "name", r.name, "route.");
                    read_str(item, "method", r.method, "route.");
                    read_str(item, "path", r.path, "route.");
                    read_str(item, "host", r.host, "route.");
                    read_str(item, "tenant", r.tenant, "route.");
                    read_str(item, "target_type", r.target_type, "route.");
                    read_str(item, "handler", r.handler_name, "route.");
                    read_str(item, "static_root", r.static_root, "route.");

                    read_bool(item, "enabled", r.enabled, "route.");
                    read_bool(item, "auth_required", r.auth_required, "route.");
                    read_bool(item, "allow_anonymous", r.allow_anonymous, "route.");
                    read_str(item, "rate_limit_policy", r.rate_limit_policy, "route.");
                    long long legacy_timeout_ms = 5000;
                    read_int(item, "timeout_ms", 1, 600000, legacy_timeout_ms, "route.");

                    if (item.contains("upstream_target")) {
                        const auto& target = item["upstream_target"];
                        read_str(target, "name", r.upstream_target.name, "route.upstream_target.");
                        long long v = r.upstream_target.timeout_ms;
                        if (read_int(target, "timeout_ms", 1, 600000, v, "route.upstream_target."))
                            r.upstream_target.timeout_ms = static_cast<int>(v);
                        v = r.upstream_target.max_retries;
                        if (read_int(target, "max_retries", 0, 100, v, "route.upstream_target."))
                            r.upstream_target.max_retries = static_cast<int>(v);
                        v = r.upstream_target.circuit_failure_threshold;
                        if (read_int(target, "circuit_failure_threshold", 1, 1000000, v, "route.upstream_target."))
                            r.upstream_target.circuit_failure_threshold = static_cast<int>(v);
                        v = r.upstream_target.circuit_recovery_timeout_ms;
                        if (read_int(target, "circuit_recovery_timeout_ms", 1, 86400000, v, "route.upstream_target."))
                            r.upstream_target.circuit_recovery_timeout_ms = static_cast<int>(v);
                    } else {
                        r.upstream_target.name = "";
                        r.upstream_target.timeout_ms = static_cast<int>(legacy_timeout_ms);
                        r.upstream_target.max_retries = 1;
                        r.upstream_target.circuit_failure_threshold = 5;
                        r.upstream_target.circuit_recovery_timeout_ms = 10000;
                    }

                    if (item.contains("allowed_api_keys")) {
                        if (!item["allowed_api_keys"].is_array()) {
                            errs.push_back("route.allowed_api_keys: expected an array");
                        } else {
                            for (const auto& k : item["allowed_api_keys"]) {
                                if (k.is_string()) r.allowed_api_keys.push_back(k.get<std::string>());
                                else errs.push_back("route.allowed_api_keys: entries must be strings");
                            }
                        }
                    }
                    if (item.contains("denied_api_keys")) {
                        if (!item["denied_api_keys"].is_array()) {
                            errs.push_back("route.denied_api_keys: expected an array");
                        } else {
                            for (const auto& k : item["denied_api_keys"]) {
                                if (k.is_string()) r.denied_api_keys.push_back(k.get<std::string>());
                                else errs.push_back("route.denied_api_keys: entries must be strings");
                            }
                        }
                    }

                    config.upstream_config.routes.push_back(r);
                }
            }
        }

        // Parse TLS settings
        if (j.contains("tls")) {
            if (!j["tls"].is_object()) {
                errs.push_back("tls: expected an object");
            } else {
                auto& tls = j["tls"];
                read_str(tls, "cert_path", config.tls.cert_path, "tls.");
                read_str(tls, "key_path", config.tls.key_path, "tls.");
            }
        }

        // Parse rate limit config
        if (j.contains("rate_limit")) {
            auto& rl = j["rate_limit"];
            long long v = config.rate_limit_config.capacity;
            if (read_int(rl, "capacity", 1, 1000000000, v, "rate_limit."))
                config.rate_limit_config.capacity = static_cast<size_t>(v);
            v = config.rate_limit_config.refill_per_second;
            if (read_int(rl, "refill_per_second", 1, 1000000000, v, "rate_limit."))
                config.rate_limit_config.refill_per_second = static_cast<size_t>(v);
        }

        // API keys may come from three sources, with later ones overriding
        // earlier ones so secrets can be kept out of the main config:
        //   1. inline "api_keys" array (legacy, discouraged)
        //   2. "api_keys_file" pointing at a JSON array file
        //   3. the GW_API_KEYS environment variable (see apply_env_keys below)
        read_str(j, "api_keys_file", config.api_keys_file, "");
        if (j.contains("api_keys")) {
            parse_api_keys_array(j["api_keys"], "api_keys", config.api_keys);
        }
        if (!config.api_keys_file.empty()) {
            std::ifstream kfs(config.api_keys_file);
            if (!kfs.is_open()) {
                errs.push_back("api_keys_file: cannot open '" + config.api_keys_file + "'");
            } else {
                nlohmann::json kj;
                try {
                    kfs >> kj;
                } catch (const std::exception& e) {
                    errs.push_back("api_keys_file: invalid JSON in '" + config.api_keys_file + "': " + e.what());
                }
                if (!kj.is_null()) {
                    config.api_keys.clear();
                    parse_api_keys_array(kj, "api_keys_file", config.api_keys);
                }
            }
        }
        apply_env_keys(config);

        return config;
    }
};
