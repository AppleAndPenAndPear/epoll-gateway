#pragma once
#include <unordered_map>
#include <string>
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

    const ApiKeyConfig* get(const std::string& key) const {
        auto it = keys_.find(key);
        if (it != keys_.end()) return &it->second;
        return nullptr;
    }

private:
    std::unordered_map<std::string, ApiKeyConfig> keys_;
};