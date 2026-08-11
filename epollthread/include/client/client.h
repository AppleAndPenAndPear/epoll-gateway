#pragma once
#include "mysocket.h"
#include <string>
#include <iostream>
#include "error_utils.h"
#include "mylogger.h"
#include "myepoll.h"
#include "clienthandler.h"

extern atomic<bool> client_stop_flag;

class Client {
private:
    bool connected_;
    Socket sock_;                   // 客户端套接字
    Epoll epoll_;                   // 每个 Client 自己的 epoll 实例
    ClientHandler handler_;
public:
    // 构造函数：连接指定IP和端口
    Client(const std::string& ip, uint16_t port);

    // 析构函数默认，Socket自动关闭描述符
    ~Client() = default;

    // 禁止拷贝
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // 允许移动
    Client(Client&& other) noexcept = default;
    Client& operator=(Client&& other) noexcept = default;

    // 获取套接字描述符
    int getFd();
    // 设置非阻塞模式
    void setnonblocking();

    void run();
};