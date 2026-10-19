#pragma once
#include "config.h"
#include <string>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstring>
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

// Advance `pos` to the next non-empty '/'-separated segment of `s`. Returns
// false once no segment remains; otherwise [*seg, *seg + *len) is the segment.
// Matching walks the two strings in place instead of splitting them into
// token vectors: the previous istringstream + std::vector<std::string>
// splitPath() ran for every route on every request and profiled hot on the
// request path (istream/locale construction, a vector alloc per segment).
inline bool next_path_segment(const std::string& s, size_t& pos,
                              const char*& seg, size_t& len) {
    while (pos < s.size() && s[pos] == '/') ++pos;
    if (pos >= s.size()) return false;
    const size_t slash = s.find('/', pos);
    const size_t stop = (slash == std::string::npos) ? s.size() : slash;
    seg = s.data() + pos;
    len = stop - pos;
    pos = stop;
    return true;
}

// Match a route pattern and extract parameters
inline bool matchRoute(const std::string& pattern, const std::string& path,std::map<std::string, std::string>& params) {
    // Wildcard prefix match: e.g. "/api/users/*" matches "/api/users/123" and "/api/users/123/orders"
    if (!pattern.empty() && pattern.back() == '*') {
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        return path.compare(0, prefix.size(), prefix) == 0;
    }

    size_t pattern_pos = 0;
    size_t path_pos = 0;
    for (;;) {
        const char* pseg = nullptr;
        const char* sseg = nullptr;
        size_t plen = 0;
        size_t slen = 0;
        const bool has_pattern = next_path_segment(pattern, pattern_pos, pseg, plen);
        const bool has_path = next_path_segment(path, path_pos, sseg, slen);
        if (has_pattern != has_path) return false;  // different segment counts
        if (!has_pattern) return true;
        if (plen >= 2 && pseg[0] == '{' && pseg[plen - 1] == '}') {
            params[std::string(pseg + 1, plen - 2)] = std::string(sseg, slen);
        } else if (plen != slen || std::memcmp(pseg, sseg, plen) != 0) {
            return false;
        }
    }
}

inline bool routeMatchesPath(const GatewayRoute& route,
                             const std::string& host,
                             const std::string& tenant,
                             const std::string& path,
                             std::map<std::string, std::string>& params) {      //404
    if (!route.enabled) {
        return false;
    }

    // "*" is the default and means "any host/tenant". Skip the lowercase +
    // port-stripping copies in that (common) case: normalize_header_value
    // allocates a fresh string, and it ran per route on every request.
    if (route.host != "*") {
        const std::string expected_host = normalize_header_value(route.host);
        if (expected_host != "*" && normalize_header_value(host) != expected_host) {
            return false;
        }
    }

    if (route.tenant != "*" && route.tenant != tenant) {
        return false;
    }

    return matchRoute(route.path, path, params);
}

inline bool routeMatchesRequest(const GatewayRoute& route,
                               const std::string& method,
                               const std::string& host,
                               const std::string& tenant,
                               const std::string& path,
                               std::map<std::string, std::string>& params) {        //405
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
