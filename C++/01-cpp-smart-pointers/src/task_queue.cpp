#include "spl/task_queue.h"

#include <stdexcept>
#include <utility>

namespace spl {

TaskQueue::TaskQueue(std::size_t worker_count)
    : stopping_(false), failed_tasks_(0) {
    if (worker_count == 0) {
        throw std::invalid_argument("TaskQueue requires at least one worker");
    }
    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.push_back(std::thread(&TaskQueue::run, this));
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i].joinable()) {
                workers_[i].join();
            }
        }
        throw;
    }
}

TaskQueue::~TaskQueue() {
    shutdown();
}

void TaskQueue::post(std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("TaskQueue::post requires a callable task");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("TaskQueue is shutting down");
        }
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
}

std::size_t TaskQueue::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

std::size_t TaskQueue::failed_tasks() const {
    return failed_tasks_.load(std::memory_order_relaxed);
}

void TaskQueue::shutdown() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i].joinable()) {
            workers_[i].join();
        }
    }
    workers_.clear();
}

void TaskQueue::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (...) {
            failed_tasks_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}
