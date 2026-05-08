#pragma once
#include <iostream>
#include <sys/socket.h>
#include <cstdint>    //uint32_t需要


using namespace std;

// 封装 epoll_wait 的返回结果
struct EpollWaitResult {
    int event_count = 0;    // 就绪的事件数量 (>=0)
    bool timeout = false;   // 是否因为超时而返回
    bool interrupted = false; // 是否被信号中断 (EINTR)

    // 方便在 if 语句中使用：判断是否有事件发生
    explicit operator bool() const { return event_count > 0; }
};

//epoll类
class Epoll{
private:
  int epollfd_;

  int setepoll_ctl(int op,int fd,struct epoll_event* ev);
public:
  Epoll();
  ~Epoll();
  Epoll(const Epoll&) = delete;
  Epoll& operator=(const Epoll&) = delete;

  Epoll(Epoll&& other) noexcept;
  Epoll& operator=(Epoll&& other) noexcept;

  void add(int fd, uint32_t events);

  void mod(int fd, uint32_t events);

  void del(int fd);

  EpollWaitResult wait(struct epoll_event* events, int maxevents, int timeout);

  int getEpollfd();

  void closeEpfd();
};