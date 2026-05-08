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
        // 一定是 EINPROGRESS（因为其他错误已抛出）
        connected_ = false;
        // 异步连接中，这里可以记录日志，后续由上层通过 epoll 监听 EPOLLOUT 事件确认连接完成
        Logger::get()->info("Client connection to {}:{} is in progress", ip, port);
        epoll_.add(sock_.getFd(), EPOLLOUT | EPOLLET | EPOLLONESHOT);
    } else {
        connected_ = true;
        Logger::get()->info("Client connected to {}:{} immediately", ip, port);
    }
}

void Client::run(){
    handler_.set_request("Hello, server!");

    // 如果连接已立刻完成，直接模拟一次可写事件
    if (connected_) {
        handler_.handle_event(sock_, epoll_, EPOLLOUT);
    }

    const int MAX_EVENTS = 4;
    epoll_event evs[MAX_EVENTS];

    int idle = 0;   //增加空闲计数，防止服务端无响应导致无限等待
    while (!handler_.is_done() && !client_stop_flag.load()) {
        auto wait_result = epoll_.wait(evs, MAX_EVENTS, 1000);
        if (!wait_result) {
            // 没有就绪事件（超时或中断）
            if (wait_result.interrupted) {
                // 你可以在这里做特殊处理，例如检查是否需要重载配置;超时期间执行定时任务
                // 目前我们只是继续循环
                continue;
            }
            if (wait_result.timeout) {
                // 每次超时检查一下是否被通知停止
                if (client_stop_flag.load()) break;
                if (++idle > 10) {   // 10 秒无响应
                    Logger::get()->error("Receive timeout");
                    break;
                }
                continue;
            }
            idle = 0;
            // 无论是超时还是中断，都继续下一轮循环
            continue;
        }
        for (int i = 0; i < wait_result.event_count; ++i) {
            if (evs[i].data.fd == sock_.getFd()){
                handler_.handle_event(sock_, epoll_, evs[i].events);
            }
        }
    }
}