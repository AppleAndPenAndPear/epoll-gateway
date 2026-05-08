#include "pool.h"

DynamicThreadPool::DynamicThreadPool(size_t min_threads, size_t max_threads,size_t scale_up_factor , size_t scale_down_factor)
        : min_threads_(min_threads), max_threads_(max_threads),scale_up_threshold_(scale_up_factor), scale_down_threshold_(scale_down_factor),stop_(false), current_threads_(0) {
    Logger::get()->info("Initializing thread pool: min threads = {}, max threads = {}, scaling up/down = {}/{}",min_threads_, max_threads_, scale_up_threshold_, scale_down_threshold_);

    std::unique_lock lock(mutex_);
    for (size_t i = 0; i < min_threads_; ++i) {
        addWorker();
    }
}

DynamicThreadPool::~DynamicThreadPool(){
    {
        std::unique_lock lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) 
            t.join();
    }
    Logger::get()->info("DynamicThreadPool destroyed, total threads joined");
}

void DynamicThreadPool::addWorker() {
    workers_.emplace_back([this] { workerLoop(); });
    ++current_threads_;
    if (auto logger = Logger::get()) {
        logger->debug("Worker added, current threads: {}", current_threads_.load());
    }
}

void DynamicThreadPool::workerLoop() {
    Logger::get()->info("creating thread,id = {}",std::hash<std::thread::id>{}(std::this_thread::get_id()));
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return stop_ || !tasks_.empty();
            })) {
                // 被唤醒是因为有任务或停止
                if (stop_ && tasks_.empty()){
                    --current_threads_;
                    Logger::get()->info("pool stop && task queue is empty, thread exists,id = {}",std::hash<std::thread::id>{}(std::this_thread::get_id()));
                    return;
                }
                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop();
                } else {
                    continue; // 可能是虚假唤醒
                }
            } else {
                // 超时，检查是否需要缩容
                if (shouldScaleDown()) {
                    Logger::get()->info("scale down: thread exiting, id = {}, remaining threads = {}",std::hash<std::thread::id>{}(std::this_thread::get_id()), current_threads_ - 1);
                    --current_threads_;
                    return; // 线程退出
                }
                continue;
            }
        }
        task();
    }
}

bool DynamicThreadPool::shouldScaleDown() const {
    return !stop_ && tasks_.empty() && current_threads_ > min_threads_;
}

size_t DynamicThreadPool::getCurrentThreads() const { return current_threads_.load(); }

bool DynamicThreadPool::shouldScaleUp() const{
    return tasks_.size() > current_threads_ * scale_up_threshold_ && current_threads_ < max_threads_;
}