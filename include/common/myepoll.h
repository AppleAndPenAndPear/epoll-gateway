#pragma once
#include <iostream>
#include <sys/socket.h>
#include <cstdint>    // needed for uint32_t


using namespace std;

// Wrapper around the result of epoll_wait
struct EpollWaitResult {
    int event_count = 0;    // number of ready events (>=0)
    bool timeout = false;   // true if the call returned due to timeout
    bool interrupted = false; // true if interrupted by a signal (EINTR)

    // Handy for use in if statements: whether any event occurred
    explicit operator bool() const { return event_count > 0; }
};

// epoll wrapper class
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