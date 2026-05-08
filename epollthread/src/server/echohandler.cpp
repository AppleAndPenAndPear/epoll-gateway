#include "echohandler.h"
#include <sys/epoll.h>
#include <unistd.h>

EchoHandler::EchoHandler(Epoll& epoll) : myepoll(epoll){ }

void EchoHandler::handle_read(shared_ptr<Socket> sock) {
    int fd = sock->getFd();
    try {       //Socket::recv 和 Socket::send 在遇到非 EAGAIN 错误时会直接抛出异常，EchoHandler 没有捕获它们会导致异常穿透到 Tcpserver::run() 的事件循环，使整个服务器崩溃
        char buffer[4096];
        while (true) {
            int n = sock->recv(buffer, sizeof(buffer), 0);
            if (n > 0) {
                send_queue[sock.get()].emplace_back(buffer, buffer + n);
                // 确保监听可写事件
                update_event(fd, EPOLLIN | EPOLLOUT);
            } else if (n == 0) {
                Logger::get()->info("EchoHandler: fd {} closed by peer", fd);
                cleanup(sock);
                return;
            } else { // n == -1
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // 数据已读完
                } else {
                    // 其他错误：抛出异常，由外层 catch 统一清理
                    throw_system_error("recv in handle_read");
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::get()->error("EchoHandler::handle_read exception on fd {}: {}", fd, e.what());
        cleanup(sock);
    }

    // 重新激活事件：依据是否有待发送数据决定监听可写
    auto it = send_queue.find(sock.get());
    if (it != send_queue.end() && !it->second.empty()) {
        myepoll.mod(fd, EPOLLIN | EPOLLOUT | EPOLLONESHOT);
    } else {
        myepoll.mod(fd, EPOLLIN | EPOLLONESHOT);
    }
}

void EchoHandler::handle_write(shared_ptr<Socket> sock) {
    int fd = sock->getFd();
    try {
        auto it = send_queue.find(sock.get());
        if (it == send_queue.end()) {
            // 没有待发送数据，应确保取消监听 EPOLLOUT
            update_event(fd, EPOLLIN);
            return;
        }
        auto& queue = send_queue[sock.get()];
        while (!queue.empty()) {
            auto& front = queue.front();
            int n = sock->send(front.data(), front.size(), 0);
            if (n > 0) {
                if (n == front.size()) {
                    queue.pop_front();  // 整块发送完毕
                } else {
                    // 只发了一部分，去掉已发送的前缀
                    front.erase(front.begin(), front.begin() + n);
                    break; // 内核缓冲区满，等下次 EPOLLOUT
                }
            } else if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // 发送缓冲区满，等待下次 EPOLLOUT
                } else {
                    throw_system_error("send in handle_write");
                }
            }
        }
        // 如果队列已空，则取消监听 EPOLLOUT，只保留 EPOLLIN
        if (queue.empty()) {
            update_event(fd, EPOLLIN);
        }
    } catch (const std::exception& e) {
        Logger::get()->error("EchoHandler::handle_write exception on fd {}: {}", fd, e.what());
        cleanup(sock);
    }

    auto it = send_queue.find(sock.get());
    if (it != send_queue.end() && !it->second.empty()) {
        myepoll.mod(fd, EPOLLIN | EPOLLOUT | EPOLLONESHOT);
    } else {
        myepoll.mod(fd, EPOLLIN | EPOLLONESHOT);
    }
}

void EchoHandler::on_connect(Socket* s){
    send_queue[s] = {};
    int fd = s->getFd();
    fd_events_[fd] = EPOLLIN;   // 假设 Tcpserver 只注册了 EPOLLIN
    Logger::get()->debug("EchoHandler: fd {} connected, initial events = EPOLLIN", fd);
}

void EchoHandler::cleanup(std::shared_ptr<Socket> sock){
    int fd = sock->getFd();
    sock->closefd();
    send_queue.erase(sock.get());
    fd_events_.erase(fd);
    Logger::get()->info("EchoHandler: fd {} cleaned up", fd);
}

void EchoHandler::update_event(int fd, uint32_t new_events){
    uint32_t old_events = current_events(fd);
    if (old_events == new_events) {
        return; // 无变化，避免系统调用
    }
    myepoll.mod(fd, new_events);
    fd_events_[fd] = new_events;
    Logger::get()->trace("EchoHandler: fd {} event changed from 0x{:x} to 0x{:x}", fd, old_events, new_events);
}

uint32_t EchoHandler::current_events(int fd) const{
    auto it = fd_events_.find(fd);
    return (it != fd_events_.end()) ? it->second : 0;
}