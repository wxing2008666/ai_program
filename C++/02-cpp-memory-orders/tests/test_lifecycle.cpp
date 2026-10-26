#include "test_framework.h"

#include <atomic>
#include <thread>
#include <vector>

#include "mo/lifecycle.h"

MO_TEST(lifecycle_allows_exactly_one_drain_winner) {
    mo::Lifecycle lifecycle;
    std::atomic<int> winners(0);
    std::vector<std::thread> workers;

    for (int i = 0; i < 8; ++i) {
        workers.push_back(std::thread([&]() {
            if (lifecycle.begin_drain()) {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }

    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }

    MO_CHECK_EQ(winners.load(std::memory_order_relaxed), 1);
    MO_CHECK(lifecycle.state() == mo::Lifecycle::State::Draining);
    MO_CHECK(!lifecycle.begin_drain());
    MO_CHECK(lifecycle.finish_drain());
    MO_CHECK(lifecycle.state() == mo::Lifecycle::State::Stopped);
}

MO_TEST(lifecycle_allows_exactly_one_finish_winner) {
    mo::Lifecycle lifecycle;
    MO_CHECK(lifecycle.begin_drain());

    std::atomic<int> winners(0);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.push_back(std::thread([&]() {
            if (lifecycle.finish_drain()) {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }

    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }

    MO_CHECK_EQ(winners.load(std::memory_order_relaxed), 1);
    MO_CHECK(lifecycle.state() == mo::Lifecycle::State::Stopped);
    MO_CHECK(!lifecycle.begin_drain());
}
