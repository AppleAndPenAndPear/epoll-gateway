#pragma once
#include <iostream>
#include "mysocket.h"
#include "myepoll.h"


class ClientHandler {
public:
    // 处理 socket 上的事件，由 Client 事件循环调用
    void handle_event(Socket& sock, Epoll& epoll, uint32_t events);

    // 设置要发送的请求（可在构造函数或外部设置）
    void set_request(const std::string& req);

    // 获取收到的响应
    std::string get_response() const;

    bool is_done() const;
private:
    enum State {
        CONNECTING,
        SENDING,
        RECEIVING,
        DONE,
        ERROR
    };
    State state_ = CONNECTING;
    std::string send_buf_;
    std::string recv_buf_;
    bool done_ = false;
};