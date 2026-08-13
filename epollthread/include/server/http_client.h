#pragma once
#include <string>
#include <map>
#include <unordered_map>

struct BackendResponse {
    int status_code;
    std::map<std::string, std::string> headers;
    std::string body;
};

BackendResponse forward_request(const std::string& host, int port,
                                const std::string& method,
                                const std::string& path,
                                const std::unordered_map<std::string, std::string>& req_headers,
                                const std::string& req_body);