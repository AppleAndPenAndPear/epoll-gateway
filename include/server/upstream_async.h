#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class SubmitStatus {
    Accepted,       // task accepted for execution
    QueueFull,      // task rejected because the queue is full
    Shutdown,       // task rejected because the executor is shutting down or has shut down
};

class BoundedAsyncUpstreamExecutor {
public:
    explicit BoundedAsyncUpstreamExecutor(size_t max_inflight = 4, size_t max_queue = 64);
    ~BoundedAsyncUpstreamExecutor();

    SubmitStatus submit(std::function<void()> task);
    SubmitStatus submit(const std::string& upstream_key, std::function<void()> task);
    void shutdown();

private:
    struct QueuedTask {
        std::string upstream_key;
        std::function<void()> task;
    };

    void worker_loop();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<QueuedTask> queue_;
    std::vector<std::thread> workers_;
    std::unordered_map<std::string, size_t> group_inflight_;

    const size_t max_inflight_;     // maximum number of tasks executing concurrently
    const size_t max_queue_;        // maximum number of tasks waiting in the queue
    const size_t max_group_inflight_;       // maximum number of tasks that can be executing or queued for the same upstream group
    std::atomic<bool> shutdown_{false};
    size_t inflight_ = 0;           // tasks currently executing in worker threads
    size_t queued_ = 0;             // tasks currently waiting in queue_
};
