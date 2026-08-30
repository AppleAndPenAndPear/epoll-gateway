#pragma once
#include <iostream>
#include "mysocket.h"
#include "pool.h"
#include "myepoll.h"
#include "http_handler.h"
#include "upstream_manager.h"

extern std::atomic<bool> stop_server_flag;

class TcpWorker {
private:
  Socket listen_sock_;
  DynamicThreadPool* pool_;
  std::atomic<bool> closed_;
  Epoll epoll_;

  enum class SSLState { HANDSHAKING, READY };
  struct ConnInfo {
    std::shared_ptr<Socket> sock;
    SSLState ssl_state;
  };
  std::unordered_map<int, ConnInfo> conns_;

  UpstreamManager upstream_manager_;
  ApiKeyManager api_key_manager_;
  std::shared_ptr<RateLimiterManager> rate_limiter_manager_;   // 跨 worker 共享
  
  HttpHandler handler_;
  std::unordered_map<int, time_t> last_active_; // 记录每个连接的最后活跃时间（秒级）
  int keepalive_timeout_; // keep-alive 超时时间（秒）
  time_t last_timeout_check_ = 0; // 上次执行超时检查的时间
  time_t last_limiter_cleanup_ = 0; // 上次清理限流器的时间
  std::unordered_map<int, std::string> client_ips_; // 记录客户端 IP 地址，便于日志输出

  void update_active(int fd);
  void check_timeout();

  SSL_CTX* ssl_ctx_ = nullptr;
public:
  TcpWorker(Socket&& listen_sock, DynamicThreadPool* pool, const Config& config, std::shared_ptr<RateLimiterManager> rate_limiter_manager);

  void run();
  void handle_accept();
  void handle_client(int fd, uint32_t events);
  void close();
};