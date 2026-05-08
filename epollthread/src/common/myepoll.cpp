#include <iostream>
#include "myepoll.h"
#include <unistd.h>
#include <sys/epoll.h>
#include "error_utils.h"
#include "mylogger.h"

Epoll::Epoll(){
  epollfd_ = epoll_create(1);
  if (epollfd_ == -1) {
    throw_system_error("epoll_create() failed");
  }
  Logger::get()->debug("creating epollfd success,epollfd = {}",epollfd_);
}

Epoll::~Epoll(){
  if(epollfd_ != -1){
    Logger::get()->debug("epollfd {} closed", epollfd_);
    close(epollfd_);
    epollfd_ = -1;
  }
}

Epoll::Epoll(Epoll&& other) noexcept : epollfd_(other.epollfd_){
  other.epollfd_ = -1;
}

Epoll& Epoll::operator=(Epoll&& other) noexcept {
  if (this != &other) {
    if (epollfd_ != -1){
      close(epollfd_);
    }
    epollfd_ = other.epollfd_;
    other.epollfd_ = -1;
  }
  return *this;
}

int Epoll::setepoll_ctl(int op,int fd,struct epoll_event* ev){
  return epoll_ctl(epollfd_, op, fd, ev);
}

// 添加文件描述符和事件
void Epoll::add(int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;
  if (setepoll_ctl(EPOLL_CTL_ADD,fd,&ev) == -1) {
    throw_system_error("epoll_ctl() ADD failed");
  }
  Logger::get()->debug("epoll_ctl() ADD fd {},events {}",fd,events);
}

// 修改文件描述符的事件
void Epoll::mod(int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;
  if (setepoll_ctl(EPOLL_CTL_MOD, fd, &ev) == -1) {
    throw_system_error("epoll_ctl() MOD failed");
  }
  Logger::get()->debug("epoll_ctl() mod fd {},events {}",fd,events);
}

// 删除文件描述符
void Epoll::del(int fd) {
  if (setepoll_ctl(EPOLL_CTL_DEL, fd, nullptr) == -1) {
    if (errno == ENOENT || errno == EBADF) {
        // fd 已经不在 epoll 中，忽略
        return;
    }
    throw_system_error("epoll_ctl() DEL failed");
  }
  Logger::get()->debug("epoll_ctl() del fd {}",fd);
}

EpollWaitResult Epoll::wait(struct epoll_event* events, int maxevents, int timeout){
  EpollWaitResult result;
  int n = epoll_wait(epollfd_, events, maxevents, timeout);
  if (n > 0) {
    // 正常情况：有事件发生
    Logger::get()->trace("epoll_wait returned {} events", n);
    result.event_count = n;
    return result;
  }
  if (n == 0) {
    // 超时：没有事件发生，等待时间耗尽
    Logger::get()->trace("epoll_wait() timeout");
    result.timeout = true;
    return result;
  }
  if (errno == EINTR) {
    result.interrupted = true;
    // 可选：记录一条低级别日志
    Logger::get()->debug("epoll_wait() interrupted by signal");
    return result;
  }
  throw_system_error("epoll_wait() failed");
}

int Epoll::getEpollfd(){
  return epollfd_;
}

void Epoll::closeEpfd(){
  if(epollfd_ != -1){
    Logger::get()->debug("epollfd {} closed", epollfd_);
    close(epollfd_);
    epollfd_ = -1;
  }
}