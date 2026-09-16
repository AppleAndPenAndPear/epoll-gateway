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

// socket wrapper
class Socket{
private:
  int fd_;
  SSL* ssl_ = nullptr;                  // SSL object
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
      // Choose to throw on failure
      throw_system_error("setsockopt() failed");
    }
    Logger::get()->debug("setsockopt() optname {} success, fd = {}",optname,fd_);
    return true;
  }

  // Convenience wrapper for SO_REUSEADDR
  bool setReuseAddr(bool enable);

  // Convenience wrapper for SO_KEEPALIVE
  bool setKeepAlive(bool enable);

  bool setReusePort(bool enable);
  
  // Set the socket to non-blocking mode
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

  // SSL support
  bool initSSL(SSL_CTX* ctx);          // attach the SSL context
  bool sslAccept();                     // perform the SSL handshake (server-side accept)
  void closeSSL();                      // gracefully shut down the SSL connection
  ssize_t sslRead(char* buf, size_t size);  // SSL read
  ssize_t sslWrite(char* buf, size_t size); // SSL write

  bool get_is_ssl_();
};