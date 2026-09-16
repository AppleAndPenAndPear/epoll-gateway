#include "echohandler.h"
#include <sys/epoll.h>
#include <unistd.h>

EchoHandler::EchoHandler(Epoll& epoll) : myepoll(epoll){ }

void EchoHandler::handle_read(shared_ptr<Socket> sock) {
    int fd = sock->getFd();
    try {       // Socket::recv and Socket::send throw directly on errors other than EAGAIN; if EchoHandler does not catch them, the exception escapes into the Tcpserver::run() event loop and crashes the whole server
        char buffer[4096];
        while (true) {
            int n = sock->recv(buffer, sizeof(buffer), 0);
            if (n > 0) {
                send_queue[sock.get()].emplace_back(buffer, buffer + n);
                // Ensure writability is monitored
                update_event(fd, EPOLLIN | EPOLLOUT);
            } else if (n == 0) {
                Logger::get()->info("EchoHandler: fd {} closed by peer", fd);
                cleanup(sock);
                return;
            } else { // n == -1
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // All data read
                } else {
                    // Other error: throw; the outer catch performs unified cleanup
                    throw_system_error("recv in handle_read");
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::get()->error("EchoHandler::handle_read exception on fd {}: {}", fd, e.what());
        cleanup(sock);
    }

    // Re-arm events: monitor writability depending on whether data is pending
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
            // No pending data; make sure EPOLLOUT is no longer monitored
            update_event(fd, EPOLLIN);
            return;
        }
        auto& queue = send_queue[sock.get()];
        while (!queue.empty()) {
            auto& front = queue.front();
            int n = sock->send(front.data(), front.size(), 0);
            if (n > 0) {
                if (n == front.size()) {
                    queue.pop_front();  // Whole block sent
                } else {
                    // Partial send: drop the already-sent prefix
                    front.erase(front.begin(), front.begin() + n);
                    break; // Kernel buffer full, wait for next EPOLLOUT
                }
            } else if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // Send buffer full, wait for next EPOLLOUT
                } else {
                    throw_system_error("send in handle_write");
                }
            }
        }
        // If the queue is empty, stop monitoring EPOLLOUT and keep only EPOLLIN
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
    fd_events_[fd] = EPOLLIN;   // Assume Tcpserver only registered EPOLLIN
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
        return; // No change, avoid the syscall
    }
    myepoll.mod(fd, new_events);
    fd_events_[fd] = new_events;
    Logger::get()->trace("EchoHandler: fd {} event changed from 0x{:x} to 0x{:x}", fd, old_events, new_events);
}

uint32_t EchoHandler::current_events(int fd) const{
    auto it = fd_events_.find(fd);
    return (it != fd_events_.end()) ? it->second : 0;
}