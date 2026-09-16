#include "http_client.h"
#include "mysocket.h"
#include "poller.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <sstream>
#include <stdexcept>

namespace {

BackendResponse make_error_response(int status_code, BackendError error, const std::string& body) {
    BackendResponse response;
    response.status_code = status_code;
    response.body = body;
    response.error = error;
    return response;
}

bool is_transient_backend_error(BackendError error) {
    switch (error) {
        case BackendError::ConnectFailed:
        case BackendError::ConnectTimeout:
        case BackendError::WriteFailed:
        case BackendError::WriteTimeout:
        case BackendError::ReadFailed:
        case BackendError::ReadTimeout:
            return true;
        default:
            return false;
    }
}

}

bool should_retry_backend_request(const std::string& method,
                                 BackendError error,
                                 int attempt_count) {
    if (attempt_count >= 2) {
        return false;
    }

    const bool idempotent = method == "GET" || method == "HEAD" || method == "OPTIONS";
    return idempotent && is_transient_backend_error(error);
}

BackendResponse forward_request(const std::string& host, int port,
                                const std::string& method,
                                const std::string& path,
                                const std::unordered_map<std::string, std::string>& req_headers,
                                const std::string& req_body,
                                int timeout_ms) {
    if (timeout_ms <= 0) {
        return make_error_response(504, BackendError::ConnectTimeout, "Gateway Timeout");
    }

    BackendResponse resp = make_error_response(502, BackendError::ConnectFailed, "Bad Gateway");
    BackendError current_phase_error = BackendError::ConnectFailed;
    try {
    Socket upstream_socket(AF_INET, SOCK_STREAM, 0);
    try {
            upstream_socket.setnonblocking();
    } catch (const std::exception&) {
        resp.error = BackendError::ConnectFailed;
        return resp;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        resp.error = BackendError::ConnectFailed;
        return resp;
    }

    bool connected = upstream_socket.connect(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (!connected) {
        const auto wait_result = Poller::wait(upstream_socket.getFd(), Poller::Event::Write, timeout_ms);
        if (wait_result == Poller::WaitResult::Timeout) {
            return make_error_response(504, BackendError::ConnectTimeout, "Gateway Timeout");
        }
        if (wait_result != Poller::WaitResult::Ready || upstream_socket.socketError() != 0) {
            resp.error = BackendError::ConnectFailed;
            return resp;
        }
    }

    // Build the HTTP request
    current_phase_error = BackendError::WriteFailed;
    std::ostringstream req_stream;
    req_stream << method << " " << path << " HTTP/1.1\r\n";
    req_stream << "Host: " << host << ":" << port << "\r\n";
    // Copy the original headers, but filter out Host (already set)
    for (const auto& [k, v] : req_headers) {
        if (k == "host") continue;
        req_stream << k << ": " << v << "\r\n";
    }
    if (!req_body.empty()) {
        req_stream << "Content-Length: " << req_body.size() << "\r\n";
    }
    req_stream << "Connection: close\r\n\r\n";
    req_stream << req_body;

    std::string request = req_stream.str();
    size_t sent = 0;
    while (sent < request.size()) {
        const auto wait_result = Poller::wait(upstream_socket.getFd(), Poller::Event::Write, timeout_ms);
        if (wait_result == Poller::WaitResult::Timeout) {
            return make_error_response(504, BackendError::WriteTimeout, "Gateway Timeout");
        }
        if (wait_result != Poller::WaitResult::Ready) {
            resp.error = BackendError::WriteFailed;
            return resp;
        }
        ssize_t written = upstream_socket.send(request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            resp.error = BackendError::WriteFailed;
            return resp;
        }
        if (written == 0) {
            resp.error = BackendError::WriteFailed;
            return resp;
        }
        sent += static_cast<size_t>(written);
    }

    // Read the response: headers and body may arrive across multiple TCP segments and a single recv would return an
    // incomplete response, so read in a loop; the request uses Connection: close, so EOF or a full Content-Length finishes it.
    current_phase_error = BackendError::ReadFailed;
    constexpr size_t kMaxResponseSize = 64 * 1024 * 1024;
    char buf[65536];
    std::string response;
    while (response.size() <= kMaxResponseSize) {
        const auto wait_result = Poller::wait(upstream_socket.getFd(), Poller::Event::Read, timeout_ms);
        if (wait_result == Poller::WaitResult::Timeout) {
            return make_error_response(504, BackendError::ReadTimeout, "Gateway Timeout");
        }
        if (wait_result != Poller::WaitResult::Ready) {
            resp.error = BackendError::ReadFailed;
            return resp;
        }
        ssize_t n = upstream_socket.recv(buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            resp.error = BackendError::ReadFailed;
            return resp;
        }
        if (n == 0) break; // Backend closed the connection per Connection: close
        response.append(buf, static_cast<size_t>(n));

        // Early completion: once headers are complete and the body reaches Content-Length, no need to wait for the peer to close
        const size_t header_end = response.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const size_t cl_pos = response.find("Content-Length: ");
            if (cl_pos != std::string::npos && cl_pos < header_end) {
                try {
                    const size_t content_length =
                        std::stoul(response.substr(cl_pos + sizeof("Content-Length: ") - 1));
                    if (response.size() >= header_end + 4 + content_length) break;
                } catch (const std::exception&) {
                    // If Content-Length parsing fails, keep reading until EOF
                }
            }
        }
    }
    if (response.empty()) {
        resp.error = BackendError::ReadFailed;
        return resp;
    }
    size_t pos = response.find(' ');
    if (pos == std::string::npos) {
        resp.error = BackendError::InvalidResponse;
        return resp;
    }
    size_t pos2 = response.find(' ', pos + 1);
    if (pos2 == std::string::npos) {
        resp.error = BackendError::InvalidResponse;
        return resp;
    }
    try {
        resp.status_code = std::stoi(response.substr(pos + 1, pos2 - pos - 1));
    } catch (const std::exception&) {
        resp.error = BackendError::InvalidResponse;
        return resp;
    }

    // Parse headers and body
    size_t header_end = response.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        resp.body = response.substr(header_end + 4);
        // Simple header parsing (best-effort)
        std::string headers_part = response.substr(0, header_end);
        size_t line_start = headers_part.find("\r\n") + 2; // Skip the status line
        while (line_start < headers_part.size()) {
            size_t line_end = headers_part.find("\r\n", line_start);
            std::string line = headers_part.substr(line_start, line_end - line_start);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 2); // Skip ": "
                resp.headers[key] = value;
            }
            if (line_end == std::string::npos) break;
            line_start = line_end + 2;
        }
    } else {
        resp.error = BackendError::InvalidResponse;
        return resp;
    }

    resp.error = BackendError::None;
    return resp;
    } catch (const std::exception&) {
        resp.error = current_phase_error;
        return resp;
    }
}