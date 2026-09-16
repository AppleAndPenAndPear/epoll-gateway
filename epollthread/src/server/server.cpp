#include "server.h"
#include <system_error>
#include <cerrno>
#include <netinet/in.h>
#include <cstring>
#include <sys/epoll.h>
#include <csignal>
#include <atomic>
#include <fcntl.h>


// The constructor only stores configuration (port, backlog, thread pool, etc.)
Tcpserver::Tcpserver(unsigned short port,int backlog,size_t min_threads, size_t max_threads,size_t scale_up_factor , size_t scale_down_factor): m_port(port),backlog_(backlog),closed(false){
  Logger::get()->info("Initializing Tcpserver on port {}", port);

  m_threadpool = make_unique<DynamicThreadPool>(min_threads,max_threads,scale_up_factor,scale_down_factor);
}

Tcpserver::~Tcpserver(){
}

void Tcpserver::start(unsigned int num_workers, const Config& config, const std::string& config_path) {
  if (num_workers == 0) {
    num_workers = std::thread::hardware_concurrency();
    if (num_workers == 0) num_workers = 4; // Fallback
  }
  Logger::get()->info("Starting {} worker threads with SO_REUSEPORT", num_workers);

  auto shared_limiter = std::make_shared<RateLimiterManager>(config.rate_limit_config);

  // Create N listen sockets and start worker threads
  for (unsigned int i = 0; i < num_workers; ++i) {
    Socket listen_sock = create_listen_sock();
    workers_.emplace_back(std::make_unique<TcpWorker>(std::move(listen_sock), m_threadpool.get(), config, shared_limiter, config_path));
    threads_.emplace_back(&TcpWorker::run, workers_.back().get());
  }

   // Wait for all workers to exit (graceful shutdown can be triggered via the closed atomic flag)
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
  // Notify all workers to exit (e.g. close their listen fds or set the atomic flag)
  for (auto& worker : workers_) {
    worker->close();  // Assume TcpWorker provides a close() method
  }
}