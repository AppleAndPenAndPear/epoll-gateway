#pragma once
#include <iostream>
#include <sys/socket.h>
#include "error_utils.h"
#include <optional>
#include <string>
#include "mylogger.h"
#include "spdlog/spdlog.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

using namespace std;

// Outcome of a non-blocking TLS handshake step
enum class SSLHandshakeStatus { COMPLETE, WANT_READ, WANT_WRITE, FAILED };

// Direction a non-blocking SSL operation is currently waiting on, so the caller
// can re-arm epoll for the right event instead of guessing.
enum class SSLWant { NONE, READ, WRITE };

// socket wrapper
class Socket{
private:
  int fd_;
  SSL* ssl_ = nullptr;                  // SSL object
  bool is_ssl_ = false;
  SSLWant last_want_ = SSLWant::NONE;   // Direction the last non-blocking SSL op wanted
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

  // Disables Nagle's algorithm (TCP_NODELAY); avoids 40ms delayed-ACK stalls
  void setnodelay();

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
  // Client-side setup: attaches a client SSL_CTX, sends SNI and pins the
  // expected hostname for certificate verification (empty hostname skips it).
  bool initSSLClient(SSL_CTX* ctx, const std::string& hostname);
  bool sslAccept();                     // perform the SSL handshake (server-side accept)
  SSLHandshakeStatus sslConnect();      // one non-blocking step of the client handshake
  void closeSSL();                      // gracefully shut down the SSL connection
  // Returns >0 bytes read, 0 on clean end of stream, or -1 with errno==EAGAIN
  // when the caller must wait for the direction reported by last_ssl_want().
  // A peer that closes without close_notify counts as a clean end of stream:
  // message completeness is decided by the caller's framing, not by TLS.
  ssize_t sslRead(char* buf, size_t size);
  ssize_t sslWrite(char* buf, size_t size); // SSL write
  // Liveness probe for pooled TLS connections: false only when the session is
  // definitively over (close_notify, bare FIN, protocol error). A pending
  // WANT_* counts as alive — there is simply nothing buffered to read yet.
  bool sslAlive();
  SSLWant last_ssl_want() const;        // direction to re-arm epoll for

  bool get_is_ssl_();
};