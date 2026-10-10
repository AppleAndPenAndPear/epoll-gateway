#pragma once
#include <unordered_map>
#include <string>
#include <algorithm>
#include "route_utils.h"
#include "config.h"

class ApiKeyManager {
public:
    explicit ApiKeyManager(const std::vector<ApiKeyConfig>& keys) {
        for (const auto& k : keys) {
            keys_[k.key] = k;
        }
    }

    bool validate(const std::string& key) const {
        return find_constant_time(key) != nullptr;
    }

    void reload(const std::vector<ApiKeyConfig>& keys) {
        keys_.clear();
        for (const auto& key : keys) {
            keys_[key.key] = key;
        }
    }

    const ApiKeyConfig* get(const std::string& key) const {
        return find_constant_time(key);
    }

    bool authorize(const std::string& key, const GatewayRoute& route,
                   const std::string& host, const std::string& tenant) const {
        const auto* config = get(key);
        if (config == nullptr || contains(route.denied_api_keys, key)) {
            return false;
        }
        if (!route.allowed_api_keys.empty() && !contains(route.allowed_api_keys, key)) {
            return false;
        }
        if (!matches_scope(config->allowed_hosts, host, true) ||
            !matches_scope(config->allowed_tenants, tenant, false)) {
            return false;
        }
        return true;
    }

private:
    // Length-independent, branch-free comparison: the loop always runs over
    // the full longer string and never returns early, so response timing does
    // not reveal how many leading bytes of a candidate key matched.
    static bool constant_time_equals(const std::string& a, const std::string& b) {
        volatile unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
        const size_t len = std::max(a.size(), b.size());
        for (size_t i = 0; i < len; ++i) {
            const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
            const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
            diff |= static_cast<unsigned char>(ca ^ cb);
        }
        return diff == 0;
    }

    // Linear scan over every configured key with a constant-time comparison.
    // Deliberately avoids unordered_map::find, whose hash lookup plus
    // early-exit key comparison would leak matched-byte prefixes via timing.
    const ApiKeyConfig* find_constant_time(const std::string& key) const {
        const ApiKeyConfig* found = nullptr;
        for (const auto& entry : keys_) {
            if (constant_time_equals(entry.first, key)) {
                found = &entry.second;
            }
        }
        return found;
    }

    static bool contains(const std::vector<std::string>& values, const std::string& value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    static bool matches_scope(const std::vector<std::string>& allowed,
                              const std::string& actual, bool host_scope) {
        if (allowed.empty()) {
            return true;
        }
        const std::string normalized_actual = host_scope ? normalize_header_value(actual) : actual;
        for (const auto& value : allowed) {
            if (value == "*" || (host_scope && normalize_header_value(value) == normalized_actual) ||
                (!host_scope && value == normalized_actual)) {
                return true;
            }
        }
        return false;
    }

    std::unordered_map<std::string, ApiKeyConfig> keys_;
};