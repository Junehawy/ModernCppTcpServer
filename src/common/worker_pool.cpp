#include "../../include/common/worker_pool.h"

#include "net/net_utils.h"


WorkerPool::WorkerPool(const size_t num_workers) {
    if (num_workers == 0) {
        throw std::invalid_argument("num_workers must be greater than zero");
    }

    workers_.reserve(num_workers);
    for (size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back([this]{worker_loop();});
    }

    NET_LOG_INFO("[WorkerPool] Started with {} threads",num_workers);
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    cond_.notify_all();
}

void WorkerPool::submit(std::function<void()> task) {
    if (!task) return;

    {
        std::lock_guard lock(mutex_);
        if (stopped_) {
            NET_LOG_WARN("[WorkerPool] Dropping task: pool is stopped");
            return;
        }
        tasks_.push(std::move(task));
    }
    cond_.notify_one();
}

size_t WorkerPool::pending_count() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

void WorkerPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cond_.wait(lock,[this]{return stopped_ || !tasks_.empty();});

            if (tasks_.empty()) return;

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        }catch (std::exception &e) {
            NET_LOG_ERROR("[WorkerPool] Exception: {}", e.what());
        }catch (...) {
            NET_LOG_ERROR("[WorkerPool] Unknown exception");
        }
    }
}