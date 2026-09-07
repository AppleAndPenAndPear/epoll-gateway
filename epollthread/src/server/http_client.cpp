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

    // 构造 HTTP 请求
    current_phase_error = BackendError::WriteFailed;
    std::ostringstream req_stream;
    req_stream << method << " " << path << " HTTP/1.1\r\n";
    req_stream << "Host: " << host << ":" << port << "\r\n";
    // 复制原始头部，但过滤掉 Host（已设）
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

    // 读取响应
    current_phase_error = BackendError::ReadFailed;
    char buf[65536];
    const auto wait_result = Poller::wait(upstream_socket.getFd(), Poller::Event::Read, timeout_ms);
    if (wait_result == Poller::WaitResult::Timeout) {
        return make_error_response(504, BackendError::ReadTimeout, "Gateway Timeout");
    }
    if (wait_result != Poller::WaitResult::Ready) {
        resp.error = BackendError::ReadFailed;
        return resp;
    }
    ssize_t n = upstream_socket.recv(buf, sizeof(buf) - 1, 0);

    if (n <= 0) {
        resp.error = BackendError::ReadFailed;
        return resp;
    }

    std::string response(buf, n);
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

    // 解析头部和 body
    size_t header_end = response.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        resp.body = response.substr(header_end + 4);
        // 简单解析头部（可忽略）
        std::string headers_part = response.substr(0, header_end);
        size_t line_start = headers_part.find("\r\n") + 2; // 跳过状态行
        while (line_start < headers_part.size()) {
            size_t line_end = headers_part.find("\r\n", line_start);
            std::string line = headers_part.substr(line_start, line_end - line_start);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 2); // 跳过 ": "
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