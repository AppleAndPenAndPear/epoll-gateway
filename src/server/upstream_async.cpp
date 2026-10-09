#include "upstream_async.h"

#include <utility>

BoundedAsyncUpstreamExecutor::BoundedAsyncUpstreamExecutor(size_t max_inflight, size_t max_queue)
    : max_inflight_(max_inflight == 0 ? 1 : max_inflight),
      max_queue_(max_queue),
      max_group_inflight_(max_inflight_ == 0 ? 1 : max_inflight_) {
    const size_t worker_count = max_inflight_ > 0 ? max_inflight_ : 1;
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&BoundedAsyncUpstreamExecutor::worker_loop, this);
    }
}

BoundedAsyncUpstreamExecutor::~BoundedAsyncUpstreamExecutor() {
    shutdown();
}

SubmitStatus BoundedAsyncUpstreamExecutor::submit(std::function<void()> task) {
    return submit("__global__", std::move(task));
}

SubmitStatus BoundedAsyncUpstreamExecutor::submit(const std::string& upstream_key, std::function<void()> task) {
    if (shutdown_.load(std::memory_order_acquire)) {
        return SubmitStatus::Shutdown;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_.load(std::memory_order_acquire)) {
        return SubmitStatus::Shutdown;
    }

    const size_t total_scheduled = inflight_ + queued_;
    if (total_scheduled >= max_inflight_ + max_queue_) {
        return SubmitStatus::QueueFull;
    }

    const size_t group_count = group_inflight_[upstream_key];
    if (group_count >= max_group_inflight_) {
        return SubmitStatus::QueueFull;
    }

    queue_.push(QueuedTask{upstream_key, std::move(task)});
    queued_ += 1;
    group_inflight_[upstream_key] += 1;
    cv_.notify_one();
    return SubmitStatus::Accepted;
}

void BoundedAsyncUpstreamExecutor::shutdown() {
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void BoundedAsyncUpstreamExecutor::worker_loop() {
    while (true) {
        QueuedTask queued_task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return shutdown_.load(std::memory_order_acquire) || !queue_.empty();
            });

            if (shutdown_.load(std::memory_order_acquire) && queue_.empty()) {
                return;
            }

            if (!queue_.empty()) {
                queued_task = std::move(queue_.front());
                queue_.pop();
                queued_ = (queued_ > 0) ? queued_ - 1 : 0;
                inflight_ += 1;
            }
        }

        if (queued_task.task) {
            queued_task.task();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inflight_ > 0) {
                inflight_ -= 1;
            }
            auto it = group_inflight_.find(queued_task.upstream_key);
            if (it != group_inflight_.end()) {
                if (it->second > 0) {
                    --it->second;
                }
                if (it->second == 0) {
                    group_inflight_.erase(it);
                }
            }
            if (shutdown_.load(std::memory_order_acquire) && queue_.empty()) {
                cv_.notify_all();
            }
        }
    }
}
