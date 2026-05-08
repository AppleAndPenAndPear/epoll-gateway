#pragma once
#include <iostream>
#include <sys/socket.h>
#include "error_utils.h"
#include <optional>
#include "mylogger.h"
#include "spdlog/spdlog.h"

using namespace std;

//管理socket
class Socket{
private:
  int fd_;
public:
  explicit Socket(int domain, int type, int protocol);
  explicit Socket(int fd);
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  int getFd();

  template<typename T>
  bool setOption(int level, int optname, const T& value) {
    if (setsockopt(fd_, level, optname, &value, sizeof(T)) == -1) {
      // 可以选择抛出异常或返回 false
      throw_system_error("setsockopt() failed");
    }
    Logger::get()->debug("setsockopt() optname {} success, fd = {}",optname,fd_);
    return true;
  }

  // 针对 SO_REUSEADDR 的便捷函数
  bool setReuseAddr(bool enable);

  // 针对 SO_KEEPALIVE 的便捷函数
  bool setKeepAlive(bool enable);

  bool setReusePort(bool enable);
  
  //设置socket非阻塞
  void setnonblocking();

  void setblocking();

  void setcloexec();

  void bind(const struct sockaddr* addr, socklen_t addrlen);

  void listen(int backlog);

  std::optional<int> accept(struct sockaddr* addr,socklen_t* addrlen);

  ssize_t recv(char* data,int size,int flags);

  ssize_t send(char* data,int size,int flags);

  bool connect(struct sockaddr* addr,int size);

  void closefd();
};