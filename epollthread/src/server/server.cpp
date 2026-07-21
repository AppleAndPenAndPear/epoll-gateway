#include "server.h"
#include <system_error>
#include <cerrno>
#include <netinet/in.h>
#include <cstring>
#include <sys/epoll.h>
#include <csignal>
#include <atomic>
#include <fcntl.h>


//构造函数只保存配置（端口、backlog、线程池等）
Tcpserver::Tcpserver(unsigned short port,int backlog,size_t min_threads, size_t max_threads,size_t scale_up_factor , size_t scale_down_factor): m_port(port),backlog_(backlog),closed(false){
  Logger::get()->info("Initializing Tcpserver on port {}", port);

  m_threadpool = make_unique<DynamicThreadPool>(min_threads,max_threads,scale_up_factor,scale_down_factor);
}

Tcpserver::~Tcpserver(){
}

void Tcpserver::start(unsigned int num_workers,const std::string& www_root,size_t cache_max_entries,size_t cache_max_file_size_mb, int keepalive_timeout) {
  if (num_workers == 0) {
    num_workers = std::thread::hardware_concurrency();
    if (num_workers == 0) num_workers = 4; // 兜底
  }
  Logger::get()->info("Starting {} worker threads with SO_REUSEPORT", num_workers);

  // 创建 N 个 listen socket 并启动 Worker 线程
  for (unsigned int i = 0; i < num_workers; ++i) {
    Socket listen_sock = create_listen_sock();
    workers_.emplace_back(std::make_unique<TcpWorker>(std::move(listen_sock), m_threadpool.get(), www_root, cache_max_entries, cache_max_file_size_mb, keepalive_timeout));
    threads_.emplace_back(&TcpWorker::run, workers_.back().get());
  }

   // 等待所有 Worker 退出（可通过 m_closed 原子变量触发优雅关闭）
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
}

Socket Tcpserver::create_listen_sock(){
  Socket s(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  s.setReusePort(true);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(m_port);
  addr.sin_addr.s_addr = INADDR_ANY;
  s.bind((const struct sockaddr*)&addr,(socklen_t)sizeof(addr));
  s.listen(backlog_);
  Logger::get()->debug("Created listen fd {} for worker", s.getFd());
  return s;
}

void Tcpserver::stop(){
  closed = true;
  // 通知所有 Worker 退出（例如关闭各自的 listen fd 或设置原子标志）
  for (auto& worker : workers_) {
    worker->close();  // 假设 TcpWorker 提供 close() 方法
  }
}