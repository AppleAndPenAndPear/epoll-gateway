#pragma once

class Poller {
public:
    enum class Event {
        Read,
        Write
    };

    enum class WaitResult {
        Ready,
        Timeout,
        Error
    };

    static WaitResult wait(int fd, Event event, int timeout_ms);
};
