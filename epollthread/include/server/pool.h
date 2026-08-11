#pragma once
#include <iostream>
#include <mutex>
#include <list>
#include <queue>
#include <functional>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <future>
#include "mylogger.h"
#include "error_utils.h"
#include "spdlog/spdlog.h"

class DynamicThreadPool {
private:
    const size_t min_threads_;
    const size_t max_threads_;
    const size_t scale_up_threshold_;   // 扩容因子，任务数/线程数
    const size_t scale_down_threshold_; // 缩容因子，目前简单使用队列空

    std::list<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;
    std::atomic<size_t> current_threads_;
public:
    DynamicThreadPool(size_t min_threads, size_t max_threads, size_t scale_up_factor, size_t scale_down_factor);

    ~DynamicThreadPool();

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result_t<F, Args...>> {
        using return_type = typename std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> res = task->get_future();
        {
            std::unique_lock lock(mutex_);
            if (stop_) {
                Logger::get()->critical("enqueue on stopped pool");
                throw std::runtime_error("enqueue on stopped pool");
            }
            tasks_.emplace([task]() { (*task)(); });

            // 检查是否需要扩容
            if (shouldScaleUp()) {
                Logger::get()->debug("scaling up: tasks = {}, current threads = {}, adding worker",tasks_.size(), current_threads_.load());
                addWorker();  // 内部会加锁并增加 current_threads_
            }
        }
        cv_.notify_one();
        return res;
    }

    size_t getCurrentThreads() const;
private:
    void addWorker();

    void workerLoop();

    bool shouldScaleDown() const;

    bool shouldScaleUp() const;
};