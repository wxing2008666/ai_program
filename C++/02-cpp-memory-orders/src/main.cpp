#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "mo/config_cell.h"
#include "mo/lifecycle.h"
#include "mo/metrics.h"
#include "mo/ref_counted.h"
#include "mo/seq_cst_gate.h"
#include "mo/spsc_ring.h"
#include "mo/tick.h"

namespace {

const std::uint64_t kTickCount = 50000;

void section(const char* title) {
    std::cout << "\n=== " << title << " ===" << std::endl;
}

}

int main() {
    std::cout << std::boolalpha;

    section("atomic layout and lock-free status");
    std::cout << "SpscRing<Tick> size=" << sizeof(mo::SpscRing<mo::Tick>)
              << ", align=" << alignof(mo::SpscRing<mo::Tick>) << std::endl;
    std::cout << "atomic<size_t> lock_free="
              << std::atomic<std::size_t>().is_lock_free()
              << ", atomic<const Config*> lock_free="
              << std::atomic<const mo::Config*>().is_lock_free() << std::endl;

    section("SPSC ring: relaxed local index + acquire/release publication");
    mo::SpscRing<mo::Tick> ring(1024);
    mo::Metrics metrics;
    long backpressure = 0;
    std::thread producer([&]() {
        for (std::uint64_t i = 0; i < kTickCount; ++i) {
            mo::Tick tick = {
                i,
                static_cast<double>(i) * 0.25,
                static_cast<std::uint32_t>(i % 100)};
            while (!ring.try_push(tick)) {
                ++backpressure;
                std::this_thread::yield();
            }
        }
        mo::Tick sentinel = {kTickCount, 0.0, 0};
        while (!ring.try_push(sentinel)) {
            std::this_thread::yield();
        }
    });

    long received = 0;
    std::uint64_t checksum = 0;
    mo::Tick tick;
    while (true) {
        if (!ring.try_pop(tick)) {
            std::this_thread::yield();
            continue;
        }
        if (tick.sequence == kTickCount) {
            break;
        }
        checksum += tick.sequence;
        metrics.record_tick(tick.sequence);
        ++received;
    }
    producer.join();

    mo::MetricsSnapshot snapshot = metrics.snapshot();
    std::cout << "received=" << received << ", backpressure=" << backpressure
              << ", checksum=" << checksum << std::endl;
    std::cout << "ticks=" << snapshot.ticks << ", drops=" << snapshot.drops
              << ", last_sequence=" << snapshot.sequence << std::endl;

    section("config publication: release store + acquire/consume loads");
    mo::ConfigCell config;
    mo::Config& fast = config.prepare(0);
    fast.max_order = 10;
    fast.risk_limit_bps = 250;
    fast.halt = false;
    config.publish(0);

    std::cout << "acquire risk_limit_bps=" << config.risk_limit_acquire()
              << ", consume max_order=" << config.max_order_consume()
              << ", halt=" << config.load_acquire()->halt << std::endl;

    section("intrusive refcount: relaxed retain + acq_rel release");
    const long alive_before = mo::RefCountedFrame::alive();
    mo::RefCountedFrame* frame = new mo::RefCountedFrame(7);
    std::atomic<int> workers_done(0);
    std::vector<std::thread> frame_workers;
    for (int i = 0; i < 4; ++i) {
        frame->retain();
        frame_workers.push_back(std::thread([frame, &workers_done]() {
            if (frame->sequence() == 7 && frame->checksum() != 0) {
                workers_done.fetch_add(1, std::memory_order_relaxed);
            }
            frame->release();
        }));
    }
    for (std::size_t i = 0; i < frame_workers.size(); ++i) {
        frame_workers[i].join();
    }
    std::cout << "workers_done=" << workers_done.load(std::memory_order_relaxed)
              << ", alive=" << mo::RefCountedFrame::alive()
              << ", use_count=" << frame->use_count() << std::endl;
    frame->release();
    std::cout << "after creator release alive=" << mo::RefCountedFrame::alive()
              << ", delta=" << (mo::RefCountedFrame::alive() - alive_before)
              << std::endl;

    section("lifecycle state machine: acq_rel compare_exchange");
    mo::Lifecycle lifecycle;
    std::atomic<int> drain_winners(0);
    std::vector<std::thread> lifecycle_workers;
    for (int i = 0; i < 4; ++i) {
        lifecycle_workers.push_back(std::thread([&]() {
            if (lifecycle.begin_drain()) {
                drain_winners.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }
    for (std::size_t i = 0; i < lifecycle_workers.size(); ++i) {
        lifecycle_workers[i].join();
    }
    std::cout << "drain_winners="
              << drain_winners.load(std::memory_order_relaxed)
              << ", state=" << static_cast<int>(lifecycle.state())
              << ", finish_ok=" << lifecycle.finish_drain()
              << ", final_state=" << static_cast<int>(lifecycle.state())
              << std::endl;

    section("seq_cst: total order prevents both-saw-nothing");
    mo::SeqCstGate gate;
    mo::SeqCstGate::Observation observation = gate.run();
    std::cout << "a_saw_b=" << observation.a_saw_b
              << ", b_saw_a=" << observation.b_saw_a
              << ", invariant=" << observation.at_least_one_saw_the_other()
              << std::endl;

    section("summary");
    std::cout << "all memory-order modules executed" << std::endl;
    return 0;
}
