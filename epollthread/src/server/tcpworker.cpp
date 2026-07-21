#include "tcpworker.h"
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>


TcpWorker::TcpWorker(Socket&& listen_sock, DynamicThreadPool* pool,const std::string& www_root,size_t cache_max,size_t cache_max_file_size_mb, int keepalive_timeout):epoll_(),listen_sock_(std::move(listen_sock)),
  pool_(pool),closed_(false),handler_(epoll_, www_root, cache_max, cache_max_file_size_mb),keepalive_timeout_(keepalive_timeout){
  epoll_.add(listen_sock_.getFd(), EPOLLIN);
  Logger::get()->info("TcpWorker created with listen fd {} by move", listen_sock_.getFd());
}

void TcpWorker::update_active(int fd) {
    last_active_[fd] = time(nullptr);
}

void TcpWorker::check_timeout() {
    time_t now = time(nullptr);
    for (auto it = last_active_.begin(); it != last_active_.end(); ) {
        int fd = it->first;
        // 如果 fd 已经无效（被关闭），直接移除记录
        if (fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
            Logger::get()->debug("check_timeout: remove stale fd {} (already closed)", fd);
            conns_.erase(fd);                    // 同步清理 conns_ 中的残留条目
            it = last_active_.erase(it);
            continue;
        }
        if (now - it->second > keepalive_timeout_) {
            // 超时，关闭连接
            Logger::get()->info("Idle timeout on fd {} (last active {}s ago), closing ...",fd, now - it->second);
            auto conn_it = conns_.find(fd);
            if (conn_it != conns_.end()) {
                handler_.cleanup(conn_it->second);
                conns_.erase(conn_it);
            }
            it = last_active_.erase(it);   // 移除定时器记录
        } else {
            ++it;
        }
    }
}

void TcpWorker::run(){
  const int MAX_EVENTS = 1024;
  epoll_event evs[MAX_EVENTS];

  while (!closed_ && !stop_server_flag.load()) {
    auto wait_result = epoll_.wait(evs, MAX_EVENTS, 1000);  // 1秒超时，可配置
    if (!wait_result) {
      // 没有就绪事件（超时或中断）
      if (wait_result.interrupted) {
        continue;
      }
      if (wait_result.timeout) {
          if (stop_server_flag.load()) break;
          check_timeout();   // 定时清理过期连接
          continue;
      }
      continue;
    }
    for (int i = 0; i < wait_result.event_count; ++i) {
      int fd = evs[i].data.fd;
      if (fd == listen_sock_.getFd()) {
        handle_accept();
      } else {
        // update_active 已在 handle_client() 中调用，此处去除重复
        handle_client(fd, evs[i].events);
      }
    }
    // 即使有事件到达，也定期检查超时（高负载下 epoll_wait 可能永不超时）
    time_t now = time(nullptr);
    if (now - last_timeout_check_ >= 1) {
      check_timeout();
      last_timeout_check_ = now;
    }
  }

  // 退出前清理所有残留连接
  for (auto& [fd, sock] : conns_) {
    handler_.cleanup(sock);
  }
  conns_.clear();
  last_active_.clear();

  Logger::get()->info("TcpWorker on fd {} exiting", listen_sock_.getFd());
}

void TcpWorker::handle_accept() {
  while (true) {
    sockaddr_in client;
    socklen_t len = sizeof(client);
    auto client_opt = listen_sock_.accept((sockaddr*)&client,&len);
    if (!client_opt) {
      break;
    }
    int clientsock = *client_opt; // 安全解引用

    // 获取客户端 IP
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client.sin_addr, ip_str, sizeof(ip_str));
    client_ips_[clientsock] = ip_str;   // 存储

    last_active_[clientsock] = time(nullptr);  // 记录活跃时间
    auto client_sock = make_shared<Socket>(clientsock);
    client_sock->setnonblocking();   // 设为非阻塞
    client_sock->setcloexec();
    epoll_.add(clientsock, EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLONESHOT);
    conns_[clientsock] = client_sock;
    handler_.on_connect(client_sock.get());   // 初始化队列
    Logger::get()->info("Worker accepted client fd {}", clientsock);
  }
}

void TcpWorker::handle_client(int fd, uint32_t events) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  auto sock = it->second;

  // 1. 只要有事件到达，就刷新活跃时间（在事件处理之前）
  update_active(fd);

  if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    // 错误或挂断事件，直接清理
    Logger::get()->info("Worker: EPOLLERR/EPOLLHUP on fd {}", fd);
    handler_.cleanup(sock);   // 清理 EchoHandler 内部状态和 epoll
    conns_.erase(fd);         // 从自已的连接表移除
    // sock 的 shared_ptr 引用计数递减，最后自动析构 Socket
    last_active_.erase(fd);          // ★ 显式擦除
    client_ips_.erase(fd); // 移除客户端 IP 记录
    return;
  }

  if (events & EPOLLIN) {
    handler_.handle_read(sock, client_ips_[fd]);  // 传入客户端 IP
  } else if (events & EPOLLOUT) {
    handler_.handle_write(sock);
  } else {
    Logger::get()->warn("Unexpected event on fd {}: {}", fd, events);
    handler_.cleanup(sock);
    conns_.erase(fd);
    last_active_.erase(fd);          // ★ 显式擦除
    client_ips_.erase(fd); // 移除客户端 IP 记录
    return;
  }

  // 3. handle_read/handle_write 内部可能因为错误或对端关闭调用了 cleanup，
  //    此时 conns_ 中已无该 fd，需同步移除定时器记录。
  if (conns_.find(fd) == conns_.end()) {
    last_active_.erase(fd);          // ★ 擦除残留记录
    Logger::get()->debug("Connection on fd {} closed during event handling", fd);
  }
}

void TcpWorker::close() {
  closed_ = true;
  // 可选：关闭 listen_fd_ 以唤醒 epoll_wait （否则可能一直阻塞）
  ::shutdown(listen_sock_.getFd(), SHUT_RD);
}