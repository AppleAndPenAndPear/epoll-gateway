#pragma once
#include <iostream>
#include "pool.h"
#include "mysocket.h"
#include "myepoll.h"
#include <memory>
//#include "echohandler.h"    // Echo only; replaced by httphandler
#include "http_handler.h"
#include "mylogger.h"
#include "error_utils.h"
#include <atomic>
#include <vector>
#include "tcpworker.h"

using namespace std;


// TCP communication server class.
class Tcpserver
{
private:
  int backlog_;
  unsigned short m_port;    // Port used by the server for communication.
  atomic<bool> closed;
  unique_ptr<DynamicThreadPool> m_threadpool;
  std::vector<std::unique_ptr<TcpWorker>> workers_;
  std::vector<std::thread> threads_;

  Socket create_listen_sock();
public:
  Tcpserver(unsigned short port,int backlog,size_t min_threads, size_t max_threads,size_t scale_up_factor , size_t scale_down_factor);
  ~Tcpserver();

  void start(unsigned int num_workers, const Config& config, const std::string& config_path);

  void stop();
};