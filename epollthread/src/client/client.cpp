#include "client.h"
#include <arpa/inet.h>
#include <cstring>
#include <system_error>
#include <sys/epoll.h>

int Client::getFd() { 
    return sock_.getFd(); 
}

void Client::setnonblocking() { 
    sock_.setnonblocking(); 
}

Client::Client(const std::string& ip, uint16_t port) : sock_(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0),epoll_(){
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &servaddr.sin_addr) <= 0) {
        throw_system_error("inet_pton failed");
    }
    bool connected = sock_.connect((struct sockaddr*)&servaddr, sizeof(servaddr));
    if (!connected) {
        // Must be EINPROGRESS (other errors already threw)
        connected_ = false;
        // Connection in progress; log here and let the upper layer watch EPOLLOUT via epoll to confirm completion
        Logger::get()->info("Client connection to {}:{} is in progress", ip, port);
        epoll_.add(sock_.getFd(), EPOLLOUT | EPOLLET | EPOLLONESHOT);
    } else {
        connected_ = true;
        Logger::get()->info("Client connected to {}:{} immediately", ip, port);
    }
}

void Client::run(){
    handler_.set_request("Hello, server!");

    // If the connection completed immediately, simulate a writable event
    if (connected_) {
        handler_.handle_event(sock_, epoll_, EPOLLOUT);
    }

    const int MAX_EVENTS = 4;
    epoll_event evs[MAX_EVENTS];

    int idle = 0;   // Idle counter to avoid waiting forever when the server is unresponsive
    while (!handler_.is_done() && !client_stop_flag.load()) {
        auto wait_result = epoll_.wait(evs, MAX_EVENTS, 1000);
        if (!wait_result) {
            // No ready events (timeout or interrupt)
            if (wait_result.interrupted) {
                // Special handling could go here, e.g. checking for a config reload; run periodic tasks during timeouts
                // For now we just continue the loop
                continue;
            }
            if (wait_result.timeout) {
                // On each timeout, check whether a stop was requested
                if (client_stop_flag.load()) break;
                if (++idle > 10) {   // No response for 10 seconds
                    Logger::get()->error("Receive timeout");
                    break;
                }
                continue;
            }
            idle = 0;
            // Continue with the next loop iteration on timeout or interrupt
            continue;
        }
        for (int i = 0; i < wait_result.event_count; ++i) {
            if (evs[i].data.fd == sock_.getFd()){
                handler_.handle_event(sock_, epoll_, evs[i].events);
            }
        }
    }
}