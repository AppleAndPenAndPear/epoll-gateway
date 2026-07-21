#pragma once
#include <iostream>
#include "mysocket.h"
#include "pool.h"
#include "myepoll.h"
#include "http_handler.h"

extern std::atomic<bool> stop_server_flag;

class TcpWorker {
private:
  Socket listen_sock_;
  DynamicThreadPool* pool_;
  std::atomic<bool> closed_;
  Epoll epoll_;
  std::unordered_map<int, std::shared_ptr<Socket>> conns_;
  HttpHandler handler_;
  std::unordered_map<int, time_t> last_active_; // 记录每个连接的最后活跃时间（秒级）
  int keepalive_timeout_; // keep-alive 超时时间（秒）
  time_t last_timeout_check_ = 0; // 上次执行超时检查的时间
  std::unordered_map<int, std::string> client_ips_; // 记录客户端 IP 地址，便于日志输出

  void update_active(int fd);
  void check_timeout();
public:
  TcpWorker(Socket&& listen_sock, DynamicThreadPool* pool,const std::string& www_root,size_t cache_max = 1024,size_t cache_max_file_size_mb = 1, int keepalive_timeout = 60);

  void run();
  void handle_accept();
  void handle_client(int fd, uint32_t events);
  void close();
};