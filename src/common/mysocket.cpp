#include "mysocket.h"
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/x509_vfy.h>


using namespace std;

Socket::Socket(int domain, int type, int protocol){
  fd_ = socket(domain, type, protocol);
  if(fd_ == -1){
    throw_system_error("socket() failed");
  }
  Logger::get()->debug("creating socket,fd = {}",fd_);
}

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket(){
  closeSSL();  // make sure SSL resources are released
  if(fd_ != -1){
    Logger::get()->debug("socket fd {} closed", fd_);
    close(fd_);
    fd_ = -1;
  }
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_), ssl_(other.ssl_), is_ssl_(other.is_ssl_) { 
  other.fd_ = -1;
  other.ssl_ = nullptr;
  other.is_ssl_ = false;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    closeSSL();
    if (fd_ != -1) close(fd_);
    fd_ = other.fd_;
    ssl_ = other.ssl_;
    is_ssl_ = other.is_ssl_;
    other.fd_ = -1;
    other.ssl_ = nullptr;
    other.is_ssl_ = false;
  }
  return *this;
}

int Socket::getFd() const {
  return fd_;
}


// Convenience wrapper for SO_REUSEADDR
bool Socket::setReuseAddr(bool enable) {
  int opt = enable ? 1 : 0;
  return setOption(SOL_SOCKET, SO_REUSEADDR, opt);
}

bool Socket::setReusePort(bool enable) {
  int opt = enable ? 1 : 0;
  return setOption(SOL_SOCKET, SO_REUSEPORT, opt);
}

// Convenience wrapper for SO_KEEPALIVE
bool Socket::setKeepAlive(bool enable) {
  int opt = enable ? 1 : 0;
  return setOption(SOL_SOCKET, SO_KEEPALIVE, opt);
}

void Socket::setnonblocking()
{
  int flags = fcntl(fd_,F_GETFL,0);

  // get the fd's current status flags.
  if  (flags == -1){
    throw_system_error("fcntl() F_GETFL failed,setnonblocking()");
  }
  if(fcntl(fd_,F_SETFL,flags|O_NONBLOCK) == -1)
    throw_system_error("fcntl() F_SETFL failed,setnonblocking()");
  Logger::get()->debug("setnonblocking fd {} success",fd_);
}

void Socket::setblocking(){
  int flags = fcntl(fd_, F_GETFL, 0);
  if (flags == -1)
    throw_system_error("fcntl() F_GETFL failed,setblocking()");
  if (fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK) == -1)
    throw_system_error("fcntl() F_SETFL failed,setblocking()");
  Logger::get()->debug("setblocking fd {} success",fd_);
}

void Socket::setcloexec(){
  if (fcntl(fd_, F_SETFD, FD_CLOEXEC) == -1)
    throw_system_error("fcntl() F_SETFD failed,setcloexec()");
  Logger::get()->debug("setcloexec fd {} success",fd_);
}

void Socket::setnodelay(){
  int one = 1;
  if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == -1)
    throw_system_error("setsockopt() TCP_NODELAY failed,setnodelay()");
  Logger::get()->debug("setnodelay fd {} success",fd_);
}

void Socket::bind(const struct sockaddr* addr, socklen_t addrlen){
  if(::bind(fd_,addr,addrlen) == -1){
    throw_system_error("bind() failed");
  }
  Logger::get()->debug("Socket fd {} bound successfully", fd_);
}

void Socket::listen(int backlog){
  if(::listen(fd_,backlog) == -1){
    throw_system_error("listen() failed");
  }
  Logger::get()->info("Socket fd {} listening with backlog {}", fd_, backlog);
}

std::optional<int> Socket::accept(struct sockaddr* addr,socklen_t* addrlen){
  int clientsock = ::accept(fd_,addr,addrlen);
  if(clientsock == -1){
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // recoverable: no pending connection; return an empty optional
      Logger::get()->trace("accept() would block on fd {}", fd_);
      return std::nullopt;
    }
    // unrecoverable: throw and let the caller handle it
    throw_system_error("accept() failed");
  }
  Logger::get()->debug("Accepted new client fd {} on listen fd {}", clientsock, fd_);
  return clientsock;
}

ssize_t Socket::recv(char* data, size_t size, int flags) {
    ssize_t n = ::recv(fd_, data, size, flags);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            Logger::get()->trace("recv() would block on fd {}", fd_);
            return -1;   // return -1 with errno == EAGAIN; the caller decides
        }
        throw_system_error("recv() failed");
    }
    if (n == 0) {
        Logger::get()->debug("recv returned 0, fd {} closed by peer", fd_);
    } else {
        Logger::get()->debug("recv from fd {}, n = {}", fd_, n);
    }
    return n;
}

ssize_t Socket::send(const char* data,size_t size,int flags){
  ssize_t n = ::send(fd_,data,size,flags);
  if (n == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPIPE) {
      Logger::get()->trace("send() would block on fd {}", fd_);
      return -1;
    }
    throw_system_error("send() failed");
  }
  Logger::get()->debug("send to fd {}, n = {}", fd_, n);
  return n;
}

bool Socket::connect(const struct sockaddr* addr,socklen_t size){
  int ret = ::connect(fd_, addr, size);
  if (ret == 0) {
    Logger::get()->debug("Socket fd {} connected immediately", fd_);
    return true;   // connected immediately
  }
  // ret == -1
  if (errno == EINPROGRESS) {
    Logger::get()->debug("Socket fd {} connection in progress", fd_);
    return false;
  }
  // other errors: throw
  throw_system_error("connect() failed");
}

int Socket::socketError() const {
  int error = 0;
  socklen_t length = sizeof(error);
  if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
    return errno;
  }
  return error;
}

void Socket::closefd(){
  if (fd_ != -1) {
    Logger::get()->debug("Explicitly closing socket fd {}", fd_);
    closeSSL();
    close(fd_);
    fd_ = -1;
  }
}

bool Socket::initSSL(SSL_CTX* ctx) {
  ssl_ = SSL_new(ctx);
  if (!ssl_) return false;
  SSL_set_fd(ssl_, fd_);
  is_ssl_ = true;
  return true;
}

bool Socket::initSSLClient(SSL_CTX* ctx, const std::string& hostname) {
  if (!initSSL(ctx)) return false;
  if (hostname.empty()) return true;
  // SNI carries a DNS name only; sending it for an IP literal is not valid.
  in_addr probe{};
  if (inet_pton(AF_INET, hostname.c_str(), &probe) != 1) {
    SSL_set_tlsext_host_name(ssl_, hostname.c_str());
  }
  // Certificate name check. This only takes effect when the context has
  // SSL_VERIFY_PEER set, so the caller must configure that first.
  SSL_set1_host(ssl_, hostname.c_str());
  return true;
}

bool Socket::sslAccept() {
  int ret = SSL_accept(ssl_);
  if (ret == 1) return true;
  int err = SSL_get_error(ssl_, ret);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    // handshake not finished; wait for events
    return false;
  }
  // other errors
  Logger::get()->error("SSL_accept failed: {}", ERR_error_string(ERR_get_error(), nullptr));
  return false;
}

SSLHandshakeStatus Socket::sslConnect() {
  ERR_clear_error();
  int ret = SSL_connect(ssl_);
  if (ret == 1) {
    last_want_ = SSLWant::NONE;
    return SSLHandshakeStatus::COMPLETE;
  }
  int err = SSL_get_error(ssl_, ret);
  if (err == SSL_ERROR_WANT_READ) {
    last_want_ = SSLWant::READ;
    return SSLHandshakeStatus::WANT_READ;
  }
  if (err == SSL_ERROR_WANT_WRITE) {
    last_want_ = SSLWant::WRITE;
    return SSLHandshakeStatus::WANT_WRITE;
  }
  last_want_ = SSLWant::NONE;
  // A failed verification is by far the most common cause here, and the generic
  // error string alone ("certificate verify failed") does not say why.
  long verify = SSL_get_verify_result(ssl_);
  std::string reason = ERR_error_string(ERR_get_error(), nullptr);
  if (verify != X509_V_OK) {
    reason += ": ";
    reason += X509_verify_cert_error_string(verify);
  }
  Logger::get()->error("SSL_connect failed: {}", reason);
  return SSLHandshakeStatus::FAILED;
}

SSLWant Socket::last_ssl_want() const {
  return last_want_;
}

void Socket::closeSSL() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
        is_ssl_ = false;
    }
}

ssize_t Socket::sslRead(char* buf, size_t size) {
    // The OpenSSL error queue is per-thread: a failure on one SSL object
    // (e.g. an aborted upstream handshake) would otherwise be picked up by
    // SSL_get_error on the NEXT object this thread touches and misreport a
    // plain WANT_READ as a fatal error.
    ERR_clear_error();
    int n = SSL_read(ssl_, buf, size);
    if (n > 0) {
        last_want_ = SSLWant::NONE;
        return n;
    }
    int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ) {
        last_want_ = SSLWant::READ;
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        last_want_ = SSLWant::WRITE;
        errno = EAGAIN;
        return -1;
    }
    last_want_ = SSLWant::NONE;
    if (err == SSL_ERROR_ZERO_RETURN) {
        return 0;  // peer sent close_notify
    }
    // The peer closed the TCP connection without close_notify. Strictly that is a
    // protocol violation, but close_notify exists to expose truncation, and every
    // caller here frames its own messages (Content-Length / chunked), which
    // detects truncation anyway. Failing instead would reject perfectly good
    // exchanges with the many HTTP stacks that never send close_notify.
    // OpenSSL 3.0 reports this as SSL_ERROR_SSL + SSL_R_UNEXPECTED_EOF_WHILE_READING;
    // OpenSSL 1.1.1 as SSL_ERROR_SYSCALL with a 0 return and an empty error queue.
    unsigned long code = ERR_peek_error();
    if (err == SSL_ERROR_SYSCALL && n == 0 && code == 0) {
        return 0;
    }
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
    if (err == SSL_ERROR_SSL && code != 0 &&
        ERR_GET_REASON(code) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
        Logger::get()->debug("peer closed without close_notify; treating as end of stream");
        return 0;
    }
#endif
    // Genuine protocol errors (bad record MAC, handshake failure, ...) stay fatal.
    Logger::get()->error("SSL_read error: {}", ERR_error_string(ERR_get_error(), nullptr));
    errno = EIO;
    return -1;
}

ssize_t Socket::sslWrite(char* buf, size_t size) {
    ERR_clear_error();  // same per-thread queue concern as sslRead
    int n = SSL_write(ssl_, buf, size);
    if (n > 0) {
        last_want_ = SSLWant::NONE;
        return n;
    }
    int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ) {
        last_want_ = SSLWant::READ;
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        last_want_ = SSLWant::WRITE;
        errno = EAGAIN;
        return -1;
    }
    last_want_ = SSLWant::NONE;
    Logger::get()->error("SSL_write error: {}", ERR_error_string(ERR_get_error(), nullptr));
    errno = EIO;
    return -1;
}

bool Socket::sslAlive() {
    char c;
    ERR_clear_error();
    // SSL_peek (unlike recv) leaves anything read available for the next
    // SSL_read, so a probe can never consume the peer's bytes.
    const int n = SSL_peek(ssl_, &c, 1);
    if (n > 0) return true;
    const int err = SSL_get_error(ssl_, n);
    // ZERO_RETURN = close_notify, SSL_ERROR_SSL = e.g. unexpected EOF (OpenSSL 3),
    // SSL_ERROR_SYSCALL = bare FIN — all mean the pooled session is dead.
    return err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
}

bool Socket::get_is_ssl_() {
    return is_ssl_;
}