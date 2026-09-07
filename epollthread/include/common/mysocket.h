#pragma once
#include <iostream>
#include <sys/socket.h>
#include "error_utils.h"
#include <optional>
#include "mylogger.h"
#include "spdlog/spdlog.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

using namespace std;

//管理socket
class Socket{
private:
  int fd_;
  SSL* ssl_ = nullptr;                  // SSL 对象
  bool is_ssl_ = false;
public:
  explicit Socket(int domain, int type, int protocol);
  explicit Socket(int fd);
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  int getFd() const;

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

  ssize_t recv(char* data,size_t size,int flags);

  ssize_t send(const char* data,size_t size,int flags);

  bool connect(const struct sockaddr* addr,socklen_t size);
  int socketError() const;

  void closefd();

  // SSL 相关
  bool initSSL(SSL_CTX* ctx);          // 关联 SSL 上下文
  bool sslAccept();                     // 执行 SSL 握手（服务端 accept）
  void closeSSL();                      // 优雅关闭 SSL 连接
  ssize_t sslRead(char* buf, size_t size);  // SSL 读
  ssize_t sslWrite(char* buf, size_t size); // SSL 写

  bool get_is_ssl_();
};