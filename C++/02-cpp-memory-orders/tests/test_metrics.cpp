#include "test_framework.h"

#include <thread>
#include <vector>

#include "mo/metrics.h"

MO_TEST(relaxed_metrics_preserve_exact_totals) {
    mo::Metrics metrics;
    std::vector<std::thread> producers;

    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        producers.push_back(std::thread([&metrics, thread_index]() {
            for (int i = 1; i <= 10000; ++i) {
                metrics.record_tick(
                    static_cast<std::uint64_t>(thread_index) * 10000 + i);
                if (i % 1000 == 0) {
                    metrics.record_drop();
                }
            }
        }));
    }

    for (std::size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }

    mo::MetricsSnapshot snapshot = metrics.snapshot();
    MO_CHECK_EQ(snapshot.ticks, 40000);
    MO_CHECK_EQ(snapshot.drops, 40);
    MO_CHECK(snapshot.sequence >= 1 && snapshot.sequence <= 40000);
}
