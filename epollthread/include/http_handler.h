#pragma once
#include "http_parser.h"
#include "mysocket.h"
#include "myepoll.h"
#include <deque>
#include <vector>
#include <unordered_map>
#include <sys/types.h>     //off_t需要，unistd.h也可

class HttpHandler {
private:
    Epoll& epoll_;
    // 每个连接的读缓冲区（用于拼接头）
    std::unordered_map<Socket*, std::string> read_bufs_;
    // 每个连接的待发送数据（响应），使用 deque<vector<char>> 优化头删
    std::unordered_map<Socket*, std::deque<std::vector<char>>> send_queues_;
    // 每个连接是否已解析出完整请求
    std::unordered_map<Socket*, bool> request_ready_;
    // 解析器实例（每个连接一个）
    std::unordered_map<Socket*, HttpParser> parsers_;
    // 已解析的请求（仅当 request_ready_ 为真时有效）
    std::unordered_map<Socket*, HttpRequest> requests_;
    // 是否保持连接
    std::unordered_map<Socket*, bool> keep_alive_;
    std::unordered_map<Socket*, int> file_fds_; //记录正在发送的文件 fd
    std::unordered_map<Socket*, off_t> file_offsets_;    // 当前发送偏移
    std::unordered_map<Socket*, off_t> file_sizes_;      // 文件总大小
    std::unordered_map<Socket*, int> resp_status_;      // 状态码
    std::unordered_map<Socket*, size_t> resp_size_;     // 将要发送的总字节数

    void send_response(Socket* sock, const HttpRequest& req);

    void send_error_response(Socket* sock, int code, const std::string& message);

    // 辅助：序列化响应头（不含 body）
    std::string headers_to_string(const HttpResponse& resp);

public:
    explicit HttpHandler(Epoll& epoll);
    void on_connect(Socket* sock);                          // 初始化
    void handle_read(std::shared_ptr<Socket> sock);         // 处理读事件
    void handle_write(std::shared_ptr<Socket> sock);
    void close_connection(std::shared_ptr<Socket> sock);
    void cleanup(std::shared_ptr<Socket> sock);
    void process_request(Socket* sock_ptr);
};