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

// TLS policy for one upstream connection. enable=false keeps the historical
// plaintext hop; the fields mirror UpstreamServer's tls_* config entries.
struct UpstreamTlsOptions {
    bool enable = false;
    std::string ca_file;       // CA bundle; empty = system default trust store;verification during connection setup TLS
    std::string server_name;   // SNI + hostname check; empty = chain-only verification
    bool skip_verify = false;  // disable verification (insecure, lab use only)
};

bool should_retry_backend_request(const std::string& method,
                                 BackendError error,
                                 int attempt_count);

BackendResponse forward_request(const std::string& host, int port,
                                const std::string& method,
                                const std::string& path,
                                const std::unordered_map<std::string, std::string>& req_headers,
                                const std::string& req_body,
                                int timeout_ms,
                                const UpstreamTlsOptions& tls = {});