#pragma once
#include <iostream>
#include <unordered_map>
#include "myepoll.h"
#include "mysocket.h"
#include <memory>
#include "mylogger.h"
#include <deque>

using namespace std;

class EchoHandler {
private:
    unordered_map<Socket*,deque<vector<char>>> send_queue;   // 每个 fd 的发送缓冲区
    Epoll& myepoll;   // epoll 文件描述符，用于修改事件
    // 记录每个 fd 当前在 epoll 中注册的事件掩码
    std::unordered_map<int, uint32_t> fd_events_;

    // 安全地修改 epoll 事件（仅在需要时调用 epoll_ctl）
    void update_event(int fd, uint32_t new_events);
    // 获取当前注册的事件（若未记录则视为0）
    uint32_t current_events(int fd) const;  
public:
    explicit EchoHandler(Epoll& epoll);
    ~EchoHandler() = default;

    void handle_read(shared_ptr<Socket> sock);
    void handle_write(shared_ptr<Socket> sock);
    void on_connect(Socket* s);
    void cleanup(std::shared_ptr<Socket> sock);
};