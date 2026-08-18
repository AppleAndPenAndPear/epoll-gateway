#pragma once
#include "http_parser.h"
#include "mysocket.h"
#include "myepoll.h"
#include <deque>
#include <vector>
#include <unordered_map>
#include <sys/types.h>     //off_t需要，unistd.h也可
#include "file_cache.h"
#include <functional>
#include <map>
#include "fd_cache.h"
#include "config.h"
#include "upstream_manager.h"
#include "rate_limiter.h"


class HttpHandler {
private:
    using RouteParams = std::map<std::string, std::string>;
    using RouteHandler = std::function<void(const HttpRequest&, HttpResponse&, const RouteParams&)>;

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
    std::string www_root_;      //文件路径
    FileCache cache_;   // 新增缓存

    void send_response(Socket* sock, const HttpRequest& req);

    void send_error_response(Socket* sock, int code, const std::string& message);

    // 辅助：序列化响应头（不含 body）
    std::string headers_to_string(const HttpResponse& resp);

    // 改为存储路由模式和处理函数，支持参数提取
    struct Route {
        std::string method;
        std::string pattern;   // 如 "/users/{id}"
        RouteHandler handler;
    };
    std::vector<Route> routes_;

    std::unordered_map<Socket*, std::string> client_ip_map_;
    std::unordered_map<Socket*, std::chrono::steady_clock::time_point> request_start_time_;
    std::unordered_map<Socket*, HttpRequest> last_requests_; // 记录上一个请求，用于日志输出
    FdCache fd_cache_;   // 文件描述符缓存
    const Config& config_;               // 引用，只读访问静态文件配置等
    
    UpstreamManager& upstream_manager_;  // 引用，用于选择后端

    // 全局限流器：跨所有 worker 共享，保证 SO_REUSEPORT 多 worker 下限流总量准确
    static std::mutex rate_limiter_mutex_;
    static std::unordered_map<std::string, std::unique_ptr<RateLimiter>> rate_limiters_;

    bool rate_limit_check(const std::string& client_ip);
public:
    explicit HttpHandler(Epoll& epoll, const Config& config,UpstreamManager& upstream_manager);
    void on_connect(Socket* sock);                          // 初始化
    void handle_read(std::shared_ptr<Socket> sock,const std::string& client_ip);         // 处理读事件
    void handle_write(std::shared_ptr<Socket> sock);
    void close_connection(std::shared_ptr<Socket> sock);
    void cleanup(std::shared_ptr<Socket> sock);
    void process_request(Socket* sock_ptr);
    void addRoute(const std::string& method, const std::string& pattern, RouteHandler handler);    // 注册路由：method 为 "GET"、"POST" 等，path 如 "/api/hello"
    std::string extract_api_key(const HttpRequest& req);
};