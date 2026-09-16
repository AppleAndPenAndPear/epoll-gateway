#pragma once
#include "config.h"
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>
#include <filesystem>

inline std::string normalize_header_value(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    // Host may include a port, but the route policy usually stores the hostname alone.
    auto port_pos = normalized.find(':');
    if (port_pos != std::string::npos) {
        return normalized.substr(0, port_pos);
    }
    return normalized;
}

// Split a path, e.g. "/users/123" -> {"users", "123"}
inline std::vector<std::string> splitPath(const std::string& path, char delimiter = '/') {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(path);
    while (std::getline(tokenStream, token, delimiter)) {
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

// Match a route pattern and extract parameters
inline bool matchRoute(const std::string& pattern, const std::string& path,std::map<std::string, std::string>& params) {
    // Wildcard prefix match: e.g. "/api/users/*" matches "/api/users/123" and "/api/users/123/orders"
    if (!pattern.empty() && pattern.back() == '*') {
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        return path.compare(0, prefix.size(), prefix) == 0;
    }

    auto patternParts = splitPath(pattern);
    auto pathParts = splitPath(path);
    if (patternParts.size() != pathParts.size()) return false;

    for (size_t i = 0; i < patternParts.size(); ++i) {
        const std::string& p = patternParts[i];
        const std::string& actual = pathParts[i];
        if (!p.empty() && p[0] == '{' && p.back() == '}') {
            std::string paramName = p.substr(1, p.size() - 2);
            params[paramName] = actual;
        } else if (p != actual) {
            return false;
        }
    }
    return true;
}

inline bool routeMatchesPath(const GatewayRoute& route,
                             const std::string& host,
                             const std::string& tenant,
                             const std::string& path,
                             std::map<std::string, std::string>& params) {
    if (!route.enabled) {
        return false;
    }

    std::string expected_host = normalize_header_value(route.host);
    std::string actual_host = normalize_header_value(host);
    if (expected_host != "*" && expected_host != actual_host) {
        return false;
    }

    std::string expected_tenant = route.tenant;
    std::string actual_tenant = tenant;
    if (expected_tenant != "*" && expected_tenant != actual_tenant) {
        return false;
    }

    return matchRoute(route.path, path, params);
}

inline bool routeMatchesRequest(const GatewayRoute& route,
                               const std::string& method,
                               const std::string& host,
                               const std::string& tenant,
                               const std::string& path,
                               std::map<std::string, std::string>& params) {
    if (!routeMatchesPath(route, host, tenant, path, params)) {
        return false;
    }
    return route.method.empty() || route.method == "*" || route.method == method;
}

inline bool is_safe_static_path(const std::string& mount_path,      // mount_path: mount point, e.g. /static
                                const std::string& request_path,
                                const std::string& root_dir,        // root_dir: static file root, e.g. /var/www
                                std::string& resolved_path) {       // resolved_path: outputs the final safe file path if valid
    std::error_code ec;
    std::filesystem::path root = std::filesystem::weakly_canonical(root_dir, ec);
    if (ec) {
        return false;
    }

    std::string suffix = request_path;
    if (!mount_path.empty() && suffix.rfind(mount_path, 0) == 0) {
        suffix = suffix.substr(mount_path.size());
    }
    if (suffix.empty()) {
        suffix = "/";
    }
    if (suffix == "/") {
        suffix = "/index.html";
    }

    // Prevent path traversal
    while (!suffix.empty() && suffix[0] == '/') {
        suffix.erase(0, 1);
    }
    if (suffix.find("..") != std::string::npos) {
        return false;
    }

    std::filesystem::path candidate = root / std::filesystem::path(suffix).lexically_normal();
    std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    std::filesystem::path canonical_candidate = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return false;
    }

    const std::string root_str = canonical_root.string();
    const std::string candidate_str = canonical_candidate.string();
    if (candidate_str.rfind(root_str, 0) != 0) {
        return false;
    }

    resolved_path = canonical_candidate.string();
    return true;
}
