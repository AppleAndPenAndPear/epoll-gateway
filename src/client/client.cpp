#include "client.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <sys/epoll.h>

namespace {
// The project only ever connects to IPv4 literals, but the client's default host
// is a name ("localhost"), so it needs a real resolver.
struct sockaddr_in resolve_ipv4(const std::string& host, uint16_t port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0) {
        throw std::runtime_error("cannot resolve host " + host + ": " + gai_strerror(rc));
    }
    struct sockaddr_in addr;
    memcpy(&addr, res->ai_addr, sizeof(addr));
    freeaddrinfo(res);
    return addr;
}

std::string build_http_request(const std::string& host, const std::string& path) {
    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Connection: close\r\n";
    req += "User-Agent: epollthread-client\r\n";
    req += "\r\n";
    return req;
}
}  // namespace

int Client::getFd() { 
    return sock_.getFd(); 
}

void Client::setnonblocking() { 
    sock_.setnonblocking(); 
}

Client::Client(const std::string& host, uint16_t port, const ClientTlsConfig& tls,
               const std::string& path)
    : host_(host), path_(path), port_(port), connected_(false),
      ssl_ctx_(nullptr, &SSL_CTX_free),
      sock_(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), epoll_() {
    if (tls.enable_tls) {
        std::string error;
        SSL_CTX* ctx = build_client_ssl_ctx(tls, &error);
        if (!ctx) {
            throw std::runtime_error("cannot create client TLS context: " + error);
        }
        ssl_ctx_.reset(ctx);
    }

    struct sockaddr_in servaddr = resolve_ipv4(host, port);
    connected_ = sock_.connect((struct sockaddr*)&servaddr, sizeof(servaddr));
    if (connected_) {
        Logger::get()->info("Client connected to {}:{} immediately", host, port);
    } else {
        // Must be EINPROGRESS (other errors already threw)
        Logger::get()->info("Client connection to {}:{} is in progress", host, port);
        // Connection in progress; let the event loop watch EPOLLOUT for completion
        epoll_.add(sock_.getFd(), EPOLLOUT | EPOLLET | EPOLLONESHOT);
    }

    if (tls.enable_tls && !sock_.initSSLClient(ssl_ctx_.get(), tls.verify_host)) {
        throw std::runtime_error("cannot attach client TLS session");
    }

    handler_.set_request(build_http_request(host, path));
}

Client::~Client() {
    // Close the socket (which frees the SSL session) before the context that
    // created it goes away.
    sock_.closefd();
    ssl_ctx_.reset();
}

int Client::run(){
    // If the connection completed immediately, simulate a writable event
    if (connected_) {
        handler_.handle_event(sock_, epoll_, EPOLLOUT);
    }

    const int MAX_EVENTS = 4;
    epoll_event evs[MAX_EVENTS];

    int idle = 0;   // Consecutive 1s timeouts; a live transfer keeps resetting it
    while (!handler_.is_done() && !client_stop_flag.load()) {
        auto wait_result = epoll_.wait(evs, MAX_EVENTS, 1000);
        if (!wait_result) {
            // No ready events (timeout or interrupt)
            if (wait_result.interrupted) {
                continue;
            }
            // A 1s timeout with nothing to do; a stop request or a silent peer
            // is the only way out at this point.
            if (client_stop_flag.load()) break;
            if (++idle > 10) {   // No activity for 10 seconds
                Logger::get()->error("Receive timeout");
                break;
            }
            continue;
        }
        idle = 0;   // Events arrived: the transfer is alive, restart the budget
        for (int i = 0; i < wait_result.event_count; ++i) {
            if (evs[i].data.fd == sock_.getFd()){
                handler_.handle_event(sock_, epoll_, evs[i].events);
            }
        }
    }

    return handler_.succeeded() ? 0 : 1;
}

std::string Client::response() const {
    return handler_.get_response();
}