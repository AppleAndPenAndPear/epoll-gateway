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
        return keys_.find(key) != keys_.end();
    }

    void reload(const std::vector<ApiKeyConfig>& keys) {
        keys_.clear();
        for (const auto& key : keys) {
            keys_[key.key] = key;
        }
    }

    const ApiKeyConfig* get(const std::string& key) const {
        auto it = keys_.find(key);
        if (it != keys_.end()) return &it->second;
        return nullptr;
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