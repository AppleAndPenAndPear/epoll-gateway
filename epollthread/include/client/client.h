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
    Socket sock_;                   // Client socket
    Epoll epoll_;                   // Epoll instance owned by each Client
    ClientHandler handler_;
public:
    // Constructor: connects to the given IP and port
    Client(const std::string& ip, uint16_t port);

    // Default destructor; Socket closes the descriptor automatically
    ~Client() = default;

    // Non-copyable
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Movable
    Client(Client&& other) noexcept = default;
    Client& operator=(Client&& other) noexcept = default;

    // Get the socket descriptor
    int getFd();
    // Set non-blocking mode
    void setnonblocking();

    void run();
};