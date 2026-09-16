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
    unordered_map<Socket*,deque<vector<char>>> send_queue;   // Send buffer per fd
    Epoll& myepoll;   // Epoll descriptor used to modify events
    // Track the event mask currently registered in epoll for each fd
    std::unordered_map<int, uint32_t> fd_events_;

    // Safely modify epoll events (calls epoll_ctl only when needed)
    void update_event(int fd, uint32_t new_events);
    // Get currently registered events (treated as 0 if untracked)
    uint32_t current_events(int fd) const;  
public:
    explicit EchoHandler(Epoll& epoll);
    ~EchoHandler() = default;

    void handle_read(shared_ptr<Socket> sock);
    void handle_write(shared_ptr<Socket> sock);
    void on_connect(Socket* s);
    void cleanup(std::shared_ptr<Socket> sock);
};