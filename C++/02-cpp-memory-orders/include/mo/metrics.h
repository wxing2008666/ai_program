#ifndef MO_METRICS_H
#define MO_METRICS_H

#include <atomic>
#include <cstdint>

namespace mo {

struct MetricsSnapshot {
    long ticks;
    long drops;
    std::uint64_t sequence;
};

class Metrics {
public:
    Metrics() : ticks_(0), drops_(0), sequence_(0) {}

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    void record_tick(std::uint64_t sequence) {
        ticks_.fetch_add(1, std::memory_order_relaxed);
        sequence_.store(sequence, std::memory_order_relaxed);
    }

    void record_drop() {
        drops_.fetch_add(1, std::memory_order_relaxed);
    }

    MetricsSnapshot snapshot() const {
        MetricsSnapshot result = {
            ticks_.load(std::memory_order_relaxed),
            drops_.load(std::memory_order_relaxed),
            sequence_.load(std::memory_order_relaxed)};
        return result;
    }

private:
    std::atomic<long> ticks_;
    std::atomic<long> drops_;
    std::atomic<std::uint64_t> sequence_;
};

}

#endif
