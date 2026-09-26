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

    // True only when a complete HTTP response was received
    bool succeeded() const;
private:
    enum State {
        CONNECTING,
        TLS_HANDSHAKING,
        SENDING,
        RECEIVING,
        DONE,
        ERROR
    };
    State state_ = CONNECTING;
    std::string send_buf_;
    std::string recv_buf_;
    bool done_ = false;

    void do_tls_handshake(Socket& sock, Epoll& epoll);
    void do_send(Socket& sock, Epoll& epoll);
    void do_receive(Socket& sock, Epoll& epoll);
};