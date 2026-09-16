#include "clienthandler.h"
#include <sys/epoll.h>
#include "mylogger.h"

void ClientHandler::handle_event(Socket& sock, Epoll& epoll, uint32_t events){
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        state_ = ERROR; done_ = true; return;
    }

    switch (state_) {
    case CONNECTING:
        if (events & EPOLLOUT) {
            // Check SO_ERROR to confirm the connection succeeded
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(sock.getFd(), SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) { state_ = ERROR; done_ = true; return; }
            state_ = SENDING;
            // Try sending immediately (or wait for the next EPOLLOUT)
            [[fallthrough]];
        }else break;
    case SENDING:
        // Send send_buf_; switch to RECEIVING once fully sent
        try {       // The state machine handles errors thrown by the send wrapper
            while (!send_buf_.empty()) {
                int n = sock.send(send_buf_.data(), send_buf_.size(), MSG_NOSIGNAL);
                if (n > 0) {
                    send_buf_.erase(0, n);
                }
                else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)){
                    epoll.mod(sock.getFd(), EPOLLOUT | EPOLLET | EPOLLONESHOT);
                    return;
                }
                else {
                    // n == -1 with a non-EAGAIN error throws from the wrapper and never reaches here
                }
            }
            // Send complete; half-close the write end
            if (::shutdown(sock.getFd(), SHUT_WR) == -1) {
                Logger::get()->error("shutdown SHUT_WR failed: {}", strerror(errno));
                state_ = ERROR; done_ = true; return;
            }
            // Switch to watching reads
            epoll.mod(sock.getFd(), EPOLLIN | EPOLLET | EPOLLONESHOT);
            state_ = RECEIVING;
        }catch (const std::exception& e) {
            Logger::get()->error("Send failed: {}", e.what());
            state_ = ERROR; done_ = true;
            return;
        }
        break;
    case RECEIVING:
        if (events & EPOLLIN) {
            try {
                char buf[4096];
                while (true) {
                    int n = sock.recv(buf, sizeof(buf), 0);
                    if (n > 0) recv_buf_.append(buf, n);
                    else if (n == 0) { state_ = DONE; done_ = true; return; }
                    else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        epoll.mod(sock.getFd(), EPOLLIN | EPOLLET | EPOLLONESHOT);
                        return;
                    }
                    else {
                        // n == -1 with a non-EAGAIN error throws from the wrapper and never reaches here
                    }
                }
            }catch (const std::exception& e) {
                Logger::get()->error("Recv failed: {}", e.what());
                state_ = ERROR; done_ = true;
            }
        }
        break;
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