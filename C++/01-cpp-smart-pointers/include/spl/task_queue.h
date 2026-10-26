#ifndef SPL_TASK_QUEUE_H
#define SPL_TASK_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace spl {

class TaskQueue {
public:
    explicit TaskQueue(std::size_t worker_count);
    ~TaskQueue();

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    void post(std::function<void()> task);
    std::size_t pending() const;
    std::size_t failed_tasks() const;
    void shutdown();

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()> > tasks_;
    std::vector<std::thread> workers_;
    bool stopping_;
    std::atomic<std::size_t> failed_tasks_;
    std::mutex lifecycle_mutex_;
};

}

#endif
