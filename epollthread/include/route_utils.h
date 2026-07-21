#pragma once
#include <vector>
#include <string>
#include <sstream>
#include <map>

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