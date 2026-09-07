#include "poller.h"

#include <chrono>
#include <cerrno>
#include <poll.h>

Poller::WaitResult Poller::wait(int fd, Event event, int timeout_ms) {
    if (fd < 0 || timeout_ms < 0) {
        return WaitResult::Error;
    }

    const short events = event == Event::Read ? POLLIN : POLLOUT;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            return WaitResult::Timeout;
        }

        pollfd descriptor{fd, events, 0};
        int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (result > 0) {
            if (descriptor.revents & (POLLNVAL | POLLERR)) {
                return WaitResult::Error;
            }
            if (descriptor.revents & (events | POLLHUP)) {
                return WaitResult::Ready;
            }
            return WaitResult::Error;
        }
        if (result == 0) {
            return WaitResult::Timeout;
        }
        if (errno != EINTR) {
            return WaitResult::Error;
        }
    }
}
