#include "clienthandler.h"
#include <sys/epoll.h>
#include <cctype>
#include "mylogger.h"

namespace {
// How the end of the response body can be determined.
enum class BodyState {
    COMPLETE,    // Content-Length framing is satisfied
    INCOMPLETE,  // Content-Length was declared but fewer bytes arrived
    NEEDS_EOF    // chunked or close-delimited: only EOF ends the body
};

// A status line plus a terminated header block is the minimum for a response
size_t header_end_of(const std::string& response) {
    if (response.compare(0, 5, "HTTP/") != 0) return std::string::npos;
    return response.find("\r\n\r\n");
}

BodyState body_state(const std::string& response, size_t header_end) {
    std::string headers = response.substr(0, header_end);
    for (char& c : headers) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // A chunked body is framed by chunk sizes, not by a byte count; treating the
    // headers alone as complete would stop before the body arrives.
    if (headers.find("transfer-encoding:") != std::string::npos) return BodyState::NEEDS_EOF;
    size_t pos = headers.find("content-length:");
    if (pos == std::string::npos) return BodyState::NEEDS_EOF;
    pos += 15;  // skip the field name
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
    unsigned long long declared = 0;
    bool digits = false;
    for (; pos < headers.size() && std::isdigit(static_cast<unsigned char>(headers[pos])); ++pos) {
        if (declared > 100000000000ULL) break;  // absurd value; do not keep growing
        declared = declared * 10 + static_cast<unsigned long long>(headers[pos] - '0');
        digits = true;
    }
    if (!digits) return BodyState::INCOMPLETE;
    return response.size() - (header_end + 4) >= declared ? BodyState::COMPLETE
                                                          : BodyState::INCOMPLETE;
}
}  // namespace

void ClientHandler::handle_event(Socket& sock, Epoll& epoll, uint32_t events){
    // EPOLLERR alone is fatal (SO_ERROR holds the cause). HUP is deliberately
    // not: a peer that closes after responding reports HUP together with IN,
    // and the bytes already in the kernel buffer are still readable — the read
    // path below drains them and decides DONE vs ERROR from the framing.
    if (events & EPOLLERR) {
        state_ = ERROR; done_ = true; return;
    }

    switch (state_) {
    case CONNECTING:
        if (!(events & EPOLLOUT)) break;
        {
            // Check SO_ERROR to confirm the connection succeeded
            const int err = sock.socketError();
            if (err != 0) {
                Logger::get()->error("Connect failed: {}", strerror(err));
                state_ = ERROR; done_ = true; return;
            }
        }
        if (sock.get_is_ssl_()) {
            state_ = TLS_HANDSHAKING;
            do_tls_handshake(sock, epoll);
            break;
        }
        state_ = SENDING;
        [[fallthrough]];
    case SENDING:
        do_send(sock, epoll);
        break;
    case TLS_HANDSHAKING:
        // The handshake takes several events; each one advances it one step.
        do_tls_handshake(sock, epoll);
        break;
    case RECEIVING:
        // Any event may be the one OpenSSL is waiting for, so this is not
        // gated on EPOLLIN.
        do_receive(sock, epoll);
        break;
    case DONE:
    case ERROR:
        break;
    }
}

// One non-blocking step of the client handshake.
void ClientHandler::do_tls_handshake(Socket& sock, Epoll& epoll) {
    const SSLHandshakeStatus status = sock.sslConnect();
    if (status == SSLHandshakeStatus::FAILED) {
        state_ = ERROR; done_ = true; return;
    }
    if (status == SSLHandshakeStatus::COMPLETE) {
        state_ = SENDING;
        do_send(sock, epoll);
        return;
    }
    // Re-arm only the direction OpenSSL is waiting on. Registering both under
    // EPOLLET | EPOLLONESHOT would spin on a socket that is permanently
    // writable while the peer has nothing to read yet.
    const uint32_t want = (status == SSLHandshakeStatus::WANT_WRITE) ? EPOLLOUT : EPOLLIN;
    epoll.mod(sock.getFd(), want | EPOLLET | EPOLLONESHOT);
}

void ClientHandler::do_send(Socket& sock, Epoll& epoll) {
    try {
        while (!send_buf_.empty()) {
            const ssize_t n = sock.get_is_ssl_()
                ? sock.sslWrite(send_buf_.data(), send_buf_.size())
                : sock.send(send_buf_.data(), send_buf_.size(), MSG_NOSIGNAL);
            if (n > 0) {
                send_buf_.erase(0, static_cast<size_t>(n));
            }
            else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)){
                epoll.mod(sock.getFd(), EPOLLOUT | EPOLLET | EPOLLONESHOT);
                return;
            }
            else {
                // Reached on, e.g., EPIPE (Socket::send maps it to -1) or an
                // SSL layer error (sslWrite returns -1 with errno=EIO). Looping
                // again would just repeat the same failure, so stop.
                Logger::get()->error("Send failed, errno={}", errno);
                state_ = ERROR; done_ = true;
                return;
            }
        }
        if (!sock.get_is_ssl_()) {
            // Tell a plaintext peer the request is complete. Over TLS this would
            // send a bare TCP FIN instead of close_notify, truncating the
            // session, so the record layer has to stay open.
            if (::shutdown(sock.getFd(), SHUT_WR) == -1) {
                Logger::get()->error("shutdown SHUT_WR failed: {}", strerror(errno));
                state_ = ERROR; done_ = true; return;
            }
        }
        // Switch to watching reads
        epoll.mod(sock.getFd(), EPOLLIN | EPOLLET | EPOLLONESHOT);
        state_ = RECEIVING;
    }catch (const std::exception& e) {
        Logger::get()->error("Send failed: {}", e.what());
        state_ = ERROR; done_ = true;
        return;
    }
}

void ClientHandler::do_receive(Socket& sock, Epoll& epoll) {
    try {
        char buf[4096];
        while (true) {
            const ssize_t n = sock.get_is_ssl_()
                ? sock.sslRead(buf, sizeof(buf))
                : sock.recv(buf, sizeof(buf), 0);
            if (n > 0) {
                recv_buf_.append(buf, static_cast<size_t>(n));
                // Finish as soon as the declared body is complete: the peer keeps
                // the connection open for its keep-alive timeout, which can be
                // longer than our deadline.
                const size_t header_end = header_end_of(recv_buf_);
                if (header_end != std::string::npos &&
                    body_state(recv_buf_, header_end) == BodyState::COMPLETE) {
                    state_ = DONE;
                    done_ = true;
                    return;
                }
            }
            else if (n == 0) {
                // EOF: without Content-Length nothing else can end the body, and a
                // truncated body must not be reported as success.
                const size_t header_end = header_end_of(recv_buf_);
                const bool complete = header_end != std::string::npos &&
                    body_state(recv_buf_, header_end) != BodyState::INCOMPLETE;
                state_ = complete ? DONE : ERROR;
                if (!complete) {
                    Logger::get()->error("Connection closed with an incomplete response");
                }
                done_ = true;
                return;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // OpenSSL may want to write as well while reading (TLS 1.3 can)
                uint32_t want = EPOLLIN;
                if (sock.get_is_ssl_() && sock.last_ssl_want() == SSLWant::WRITE) {
                    want = EPOLLOUT;
                }
                epoll.mod(sock.getFd(), want | EPOLLET | EPOLLONESHOT);
                return;
            }
            else {
                // sslRead maps fatal errors to -1 with errno=EIO (the SSL layer
                // does not throw), so a wrong assumption here would spin forever.
                Logger::get()->error("Recv failed, errno={}", errno);
                state_ = ERROR; done_ = true;
                return;
            }
        }
    }catch (const std::exception& e) {
        Logger::get()->error("Recv failed: {}", e.what());
        state_ = ERROR; done_ = true;
    }
}

void ClientHandler::set_request(const std::string& req){
    send_buf_ = req;
}

// Get the received response
std::string ClientHandler::get_response() const { 
    return recv_buf_;
}

bool ClientHandler::is_done() const { 
    return done_;
}

bool ClientHandler::succeeded() const {
    return state_ == DONE;
}