#pragma once
#include "mysocket.h"
#include "client_tls.h"
#include <string>
#include <cstdint>
#include <memory>
#include <iostream>
#include "error_utils.h"
#include "mylogger.h"
#include "myepoll.h"
#include "clienthandler.h"

extern atomic<bool> client_stop_flag;

class Client {
private:
    std::string host_;              // Host as given by the caller (used in the Host: header)
    std::string path_;              // Request path
    uint16_t port_;
    bool connected_;
    // Declared before sock_ so the SSL session (owned by Socket) is destroyed
    // before the context it was created from.
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ssl_ctx_;
    Socket sock_;                   // Client socket
    Epoll epoll_;                   // Epoll instance owned by each Client
    ClientHandler handler_;
public:
    // Constructor: connects to host:port, optionally over TLS, and prepares the
    // GET request for path. Throws std::runtime_error / std::system_error on
    // setup failure.
    Client(const std::string& host, uint16_t port, const ClientTlsConfig& tls,
           const std::string& path = "/");

    // Closes the socket first, then releases the context
    ~Client();

    // Non-copyable
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Get the socket descriptor
    int getFd();
    // Set non-blocking mode
    void setnonblocking();

    // Drives the event loop until a response is complete or the attempt fails.
    // Returns 0 on a complete HTTP response, 1 otherwise.
    int run();

    // Raw response bytes received so far
    std::string response() const;
};