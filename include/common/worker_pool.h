#pragma once
#include <condition_variable>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

class WorkerPool {
public:
    explicit WorkerPool(size_t num_workers);
    ~WorkerPool();

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    void submit(std::function<void()> task);

    size_t thread_count() const{return workers_.size();}
    size_t pending_count() const;

private:
    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> stopped_{false};

    void worker_loop();
};