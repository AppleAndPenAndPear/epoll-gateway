#pragma once
#include <iostream>
#include "mysocket.h"
#include "myepoll.h"


class ClientHandler {
public:
    // Handle events on the socket; called by the Client event loop
    void handle_event(Socket& sock, Epoll& epoll, uint32_t events);

    // Set the request to send (set in the constructor or externally)
    void set_request(const std::string& req);

    // Get the received response
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