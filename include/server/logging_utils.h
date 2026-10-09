#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "config.h"

namespace logging_utils {

inline std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline int hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

inline std::string decode_query_name(const std::string& name) {
    std::string decoded;
    decoded.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '+' ) {
            decoded.push_back(' ');
        } else if (name[i] == '%' && i + 2 < name.size()) {
            const int high = hex_value(static_cast<unsigned char>(name[i + 1]));
            const int low = hex_value(static_cast<unsigned char>(name[i + 2]));
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                decoded.push_back(name[i]);
            }
        } else {
            decoded.push_back(name[i]);
        }
    }
    return lowercase(std::move(decoded));
}

inline bool contains_name(const std::vector<std::string>& names, const std::string& name) {
    const std::string normalized = lowercase(name);
    return std::any_of(names.begin(), names.end(), [&normalized](const std::string& candidate) {
        return lowercase(candidate) == normalized;
    });
}

inline std::string redact_header_value(const std::string& name,
                                      const std::string& value,
                                      const LoggingConfig& config) {
    if (config.redact_sensitive_headers && !value.empty() &&
        contains_name(config.sensitive_headers, name)) {
        return "[redacted]";
    }
    return value;
}

inline std::string redact_api_key(const std::string& key) {
    return key.empty() ? "anonymous" : "[redacted]";
}

inline std::string redact_request_target(const std::string& target,
                                         const LoggingConfig& config) {
    if (!config.redact_sensitive_query_params) return target;
    const size_t query_start = target.find('?');
    if (query_start == std::string::npos) return target;

    std::string redacted = target.substr(0, query_start + 1);
    size_t position = query_start + 1;
    while (position < target.size()) {
        const size_t separator = target.find('&', position);
        const size_t end = separator == std::string::npos ? target.size() : separator;
        const std::string part = target.substr(position, end - position);
        const size_t equals = part.find('=');
        if (equals != std::string::npos &&
            contains_name(config.sensitive_query_params, decode_query_name(part.substr(0, equals)))) {
            redacted += part.substr(0, equals + 1) + "[redacted]";
        } else {
            redacted += part;
        }
        if (separator == std::string::npos) break;
        redacted.push_back('&');
        position = separator + 1;
    }
    return redacted;
}

inline std::string redact_request_target(const std::string& path,
                                         const std::string& query,
                                         const LoggingConfig& config) {
    return redact_request_target(path + (query.empty() ? "" : "?" + query), config);
}

}  // namespace logging_utils