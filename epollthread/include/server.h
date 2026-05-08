#pragma once
#include <iostream>
#include "pool.h"
#include "mysocket.h"
#include "myepoll.h"
#include <memory>
#include "echohandler.h"
#include "mylogger.h"
#include "error_utils.h"
#include <atomic>
#include <vector>


using namespace std;

extern std::atomic<bool> stop_server_flag;

class TcpWorker {
private:
  Socket listen_sock_;
  DynamicThreadPool* pool_;
  std::atomic<bool> closed_;
  Epoll epoll_;
  std::unordered_map<int, std::shared_ptr<Socket>> conns_;
  EchoHandler handler_;
public:
  TcpWorker(Socket&& listen_sock, DynamicThreadPool* pool);

  void run();
  void handle_accept();
  void handle_client(int fd, uint32_t events);
  void close();
};

// TCP通讯的服务端类。
class Tcpserver
{
private:
  int backlog_;
  unsigned short m_port;    // 服务端用于通讯的端口。
  atomic<bool> closed;
  unique_ptr<DynamicThreadPool> m_threadpool;
  std::vector<std::unique_ptr<TcpWorker>> workers_;
  std::vector<std::thread> threads_;

  Socket create_listen_sock();
public:
  Tcpserver(unsigned short port,int backlog,size_t min_threads, size_t max_threads,size_t scale_up_factor , size_t scale_down_factor);
  ~Tcpserver();

  void start(unsigned int num_workers = 0);

  void stop();
};