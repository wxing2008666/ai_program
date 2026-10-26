#include <future>
#include <memory>
#include <stdexcept>

#include "spl/pipeline.h"
#include "spl/task_queue.h"
#include "test_framework.h"

SPL_TEST(async_strong_capture_extends_lifetime_until_task_runs) {
    spl::Tracked<spl::NodeTag>::reset_counters();
    spl::TaskQueue queue(1);
    std::shared_ptr<spl::Node> node = spl::make_node("strong");
    std::weak_ptr<spl::Node> observer = node;

    std::promise<void> gate;
    std::shared_future<void> gate_future = gate.get_future().share();
    std::promise<void> done;
    std::future<void> done_future = done.get_future();

    queue.post([gate_future]() { gate_future.wait(); });
    queue.post([node, &done]() {
        (void)node;
        done.set_value();
    });

    node.reset();
    SPL_CHECK(!observer.expired());
    SPL_CHECK_EQ(spl::Tracked<spl::NodeTag>::alive(), 1);

    gate.set_value();
    done_future.wait();
    queue.shutdown();

    SPL_CHECK(observer.expired());
    SPL_CHECK_EQ(spl::Tracked<spl::NodeTag>::alive(), 0);
}

SPL_TEST(async_weak_capture_cancels_after_owner_is_gone) {
    spl::Tracked<spl::NodeTag>::reset_counters();
    spl::TaskQueue queue(1);
    std::shared_ptr<spl::Node> node = spl::make_node("weak");
    std::weak_ptr<spl::Node> observer = node;
    std::weak_ptr<spl::Node> weak_node = node;

    std::promise<void> gate;
    std::shared_future<void> gate_future = gate.get_future().share();
    std::promise<void> cancelled;
    std::future<void> cancelled_future = cancelled.get_future();
    bool ran = false;

    queue.post([gate_future]() { gate_future.wait(); });
    queue.post([weak_node, &cancelled, &ran]() {
        std::shared_ptr<spl::Node> locked = weak_node.lock();
        if (!locked) {
            ran = true;
            cancelled.set_value();
        }
    });

    node.reset();
    SPL_CHECK(observer.expired());
    gate.set_value();
    cancelled_future.wait();
    queue.shutdown();

    SPL_CHECK(ran);
    SPL_CHECK_EQ(spl::Tracked<spl::NodeTag>::alive(), 0);
}

SPL_TEST(task_queue_isolates_task_failures_and_rejects_new_work) {
    spl::TaskQueue queue(1);
    queue.post([]() { throw std::runtime_error("task failed"); });

    std::promise<void> done;
    std::future<void> done_future = done.get_future();
    queue.post([&done]() { done.set_value(); });
    done_future.wait();

    SPL_CHECK_EQ(queue.failed_tasks(), 1);
    queue.shutdown();
    SPL_CHECK_THROW(queue.post([]() {}), std::runtime_error);
}

SPL_TEST(task_queue_validates_configuration) {
    SPL_CHECK_THROW(spl::TaskQueue queue(0), std::invalid_argument);
    spl::TaskQueue queue(1);
    SPL_CHECK_THROW(queue.post(std::function<void()>()), std::invalid_argument);
}
