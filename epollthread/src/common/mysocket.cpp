#include "mysocket.h"
#include <unistd.h>
#include <fcntl.h>


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
  closeSSL();  // 确保 SSL 资源被释放
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


// 针对 SO_REUSEADDR 的便捷函数
bool Socket::setReuseAddr(bool enable) {
  int opt = enable ? 1 : 0;
  return setOption(SOL_SOCKET, SO_REUSEADDR, opt);
}

bool Socket::setReusePort(bool enable) {
  int opt = enable ? 1 : 0;
  return setOption(SOL_SOCKET, SO_REUSEPORT, opt);
}

// 针对 SO_KEEPALIVE 的便捷函数
bool Socket::setKeepAlive(bool enable) {
  int opt = enable ? 1 : 0;
  return setOption(SOL_SOCKET, SO_KEEPALIVE, opt);
}

void Socket::setnonblocking()
{
  int flags = fcntl(fd_,F_GETFL,0);

  // 获取fd的状态。
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
      // 可恢复错误：无新连接，返回空 optional
      Logger::get()->trace("accept() would block on fd {}", fd_);
      return std::nullopt;
    }
    // 不可恢复的错误：抛出异常，交由上层处理
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
            return -1;   // 返回 -1 但 errno 为 EAGAIN，由上层判断
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
    return true;   // 立即连接成功
  }
  // ret == -1
  if (errno == EINPROGRESS) {
    Logger::get()->debug("Socket fd {} connection in progress", fd_);
    return false;
  }
  // 其他错误，抛出异常
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

bool Socket::sslAccept() {
  int ret = SSL_accept(ssl_);
  if (ret == 1) return true;
  int err = SSL_get_error(ssl_, ret);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    // 握手未完成，需要等待事件
    return false;
  }
  // 其他错误
  Logger::get()->error("SSL_accept failed: {}", ERR_error_string(ERR_get_error(), nullptr));
  return false;
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
    int n = SSL_read(ssl_, buf, size);
    if (n > 0) return n;
    int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN) {
        return 0;  // 对端关闭
    }
    // 其他错误
    Logger::get()->error("SSL_read error: {}", ERR_error_string(ERR_get_error(), nullptr));
    errno = EIO;
    return -1;
}

ssize_t Socket::sslWrite(char* buf, size_t size) {
    int n = SSL_write(ssl_, buf, size);
    if (n > 0) return n;
    int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    Logger::get()->error("SSL_write error: {}", ERR_error_string(ERR_get_error(), nullptr));
    errno = EIO;
    return -1;
}

bool Socket::get_is_ssl_() {
    return is_ssl_;
}