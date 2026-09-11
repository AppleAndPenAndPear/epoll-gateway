#pragma once
#include <string>
#include <map>
#include <unordered_map>

struct BackendResponse {
    int status_code = 502;
    std::map<std::string, std::string> headers;
    std::string body;
    enum class Error {
        None,
        ConnectFailed,
        ConnectTimeout,
        WriteFailed,
        WriteTimeout,
        ReadFailed,
        ReadTimeout,
        InvalidResponse
    } error = Error::None;
};

using BackendError = BackendResponse::Error;

bool should_retry_backend_request(const std::string& method,
                                 BackendError error,
                                 int attempt_count);

BackendResponse forward_request(const std::string& host, int port,
                                const std::string& method,
                                const std::string& path,
                                const std::unordered_map<std::string, std::string>& req_headers,
                                const std::string& req_body,
                                int timeout_ms);