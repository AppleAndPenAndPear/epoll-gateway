#include "server.h"
#include <system_error>
#include <cerrno>
#include <netinet/in.h>
#include <cstring>
#include <sys/epoll.h>
#include <csignal>
#include <atomic>


TcpWorker::TcpWorker(Socket&& listen_sock, DynamicThreadPool* pool):epoll_(),listen_sock_(std::move(listen_sock)),pool_(pool),closed_(false),handler_(epoll_){
  epoll_.add(listen_sock_.getFd(), EPOLLIN);
  Logger::get()->info("TcpWorker created with listen fd {} by move", listen_sock_.getFd());
}

void TcpWorker::run(){
  const int MAX_EVENTS = 1024;
  epoll_event evs[MAX_EVENTS];

  while (!closed_ && !stop_server_flag.load()) {
    auto wait_result = epoll_.wait(evs, MAX_EVENTS, 1000);  // 1秒超时，可配置
    if (!wait_result) {
      // 没有就绪事件（超时或中断）
      if (wait_result.interrupted) {
        // 你可以在这里做特殊处理，例如检查是否需要重载配置;超时期间执行定时任务
        // 目前我们只是继续循环
        continue;
      }
      if (wait_result.timeout) {
          // 每次超时检查一下是否被通知停止
          if (stop_server_flag.load()) break;
          continue;
      }
      // 无论是超时还是中断，都继续下一轮循环
      continue;
    }
    for (int i = 0; i < wait_result.event_count; ++i) {
      int fd = evs[i].data.fd;
      if (fd == listen_sock_.getFd()) {
        handle_accept();
      } else {
        handle_client(fd, evs[i].events);
      }
    }
  }

  // 退出后，可记录 Worker 结束日志
  Logger::get()->info("TcpWorker on fd {} exiting", listen_sock_.getFd());
}

void TcpWorker::handle_accept() {
  while (true) {
    sockaddr_in client;
    socklen_t len = sizeof(client);
    auto client_opt = listen_sock_.accept((sockaddr*)&client,&len);
    if (!client_opt) {
      break;
    }
    int clientsock = *client_opt; // 安全解引用
    auto client_sock = make_shared<Socket>(clientsock);
    client_sock->setnonblocking();   // 设为非阻塞
    client_sock->setcloexec();
    epoll_.add(clientsock, EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLONESHOT);
    conns_[clientsock] = client_sock;
    handler_.on_connect(client_sock.get());   // 初始化队列
    Logger::get()->info("Worker accepted client fd {}", clientsock);
  }
}

void TcpWorker::handle_client(int fd, uint32_t events) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  auto sock = it->second;

  if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    // 错误或挂断事件，直接清理
    Logger::get()->info("Worker: EPOLLERR/EPOLLHUP on fd {}", fd);
    handler_.cleanup(sock);   // 清理 EchoHandler 内部状态和 epoll
    conns_.erase(fd);         // 从自已的连接表移除
    // sock 的 shared_ptr 引用计数递减，最后自动析构 Socket
    return;
  }

  if (events & EPOLLIN) {
    handler_.handle_read(sock);
  } else if (events & EPOLLOUT) {
    handler_.handle_write(sock);
  } else {
    Logger::get()->warn("Unexpected event on fd {}: {}", fd, events);
    handler_.cleanup(sock);
    conns_.erase(fd);
  }
}

void TcpWorker::close() {
  closed_ = true;
  // 可选：关闭 listen_fd_ 以唤醒 epoll_wait （否则可能一直阻塞）
  ::shutdown(listen_sock_.getFd(), SHUT_RD);
}

//构造函数只保存配置（端口、backlog、线程池等）
Tcpserver::Tcpserver(unsigned short port,int backlog,size_t min_threads, size_t max_threads,size_t scale_up_factor , size_t scale_down_factor): m_port(port),backlog_(backlog),closed(false){
  Logger::get()->info("Initializing Tcpserver on port {}", port);

  m_threadpool = make_unique<DynamicThreadPool>(min_threads,max_threads,scale_up_factor,scale_down_factor);
}

Tcpserver::~Tcpserver(){
}

void Tcpserver::start(unsigned int num_workers) {
  if (num_workers == 0) {
    num_workers = std::thread::hardware_concurrency();
    if (num_workers == 0) num_workers = 4; // 兜底
  }
  Logger::get()->info("Starting {} worker threads with SO_REUSEPORT", num_workers);

  // 创建 N 个 listen socket 并启动 Worker 线程
  for (unsigned int i = 0; i < num_workers; ++i) {
    Socket listen_sock = create_listen_sock();
    workers_.emplace_back(std::make_unique<TcpWorker>(std::move(listen_sock), m_threadpool.get()));
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