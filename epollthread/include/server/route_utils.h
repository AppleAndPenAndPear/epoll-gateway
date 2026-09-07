#pragma once
#include "config.h"
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>

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

// 分割路径，如 "/users/123" -> {"users", "123"}
inline std::vector<std::string> splitPath(const std::string& path, char delimiter = '/') {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(path);
    while (std::getline(tokenStream, token, delimiter)) {
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

// 匹配路由模式，提取参数
inline bool matchRoute(const std::string& pattern, const std::string& path,std::map<std::string, std::string>& params) {
    // 通配符前缀匹配：如 "/api/users/*" 可匹配 "/api/users/123"、"/api/users/123/orders"
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

inline bool routeMatchesRequest(const GatewayRoute& route,
                               const std::string& method,
                               const std::string& host,
                               const std::string& tenant,
                               const std::string& path,
                               std::map<std::string, std::string>& params) {
    if (!route.enabled) {
        return false;
    }

    if (!route.method.empty() && route.method != "*" && route.method != method) {
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
