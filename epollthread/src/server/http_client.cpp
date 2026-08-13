#include "http_client.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>

BackendResponse forward_request(const std::string& host, int port,
                                const std::string& method,
                                const std::string& path,
                                const std::unordered_map<std::string, std::string>& req_headers,
                                const std::string& req_body) {
    BackendResponse resp;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return resp;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return resp; // 连接失败，返回空响应
    }

    // 构造 HTTP 请求
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
    send(fd, request.data(), request.size(), 0);

    // 读取响应
    char buf[65536];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);

    if (n <= 0) return resp;

    std::string response(buf, n);
    size_t pos = response.find(' ');
    if (pos == std::string::npos) return resp;
    size_t pos2 = response.find(' ', pos + 1);
    if (pos2 == std::string::npos) return resp;
    resp.status_code = std::stoi(response.substr(pos + 1, pos2 - pos - 1));

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
    }

    return resp;
}