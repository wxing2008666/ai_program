#include "test_framework.h"

#include <cstdint>
#include <thread>

#include "mo/spsc_ring.h"
#include "mo/tick.h"

namespace {

const std::uint64_t kTickCount = 20000;

}

MO_TEST(spsc_ring_rejects_invalid_capacity) {
    MO_CHECK_THROW(mo::SpscRing<int>(0));
    MO_CHECK_THROW(mo::SpscRing<int>(3));
    MO_CHECK_THROW(mo::SpscRing<int>(6));
}

MO_TEST(spsc_ring_reports_full_and_empty) {
    mo::SpscRing<int> ring(4);
    MO_CHECK(ring.try_push(10));
    MO_CHECK(ring.try_push(20));
    MO_CHECK(ring.try_push(30));
    MO_CHECK(ring.try_push(40));
    MO_CHECK(!ring.try_push(50));

    int value = 0;
    MO_CHECK(ring.try_pop(value));
    MO_CHECK_EQ(value, 10);
    MO_CHECK(ring.try_push(50));

    for (int expected = 20; expected <= 50; expected += 10) {
        MO_CHECK(ring.try_pop(value));
        MO_CHECK_EQ(value, expected);
    }
    MO_CHECK(!ring.try_pop(value));
}

MO_TEST(spsc_ring_transfers_ticks_in_order) {
    mo::SpscRing<mo::Tick> ring(1024);
    std::thread producer([&]() {
        for (std::uint64_t i = 0; i < kTickCount; ++i) {
            mo::Tick tick = {i, static_cast<double>(i) * 0.5, static_cast<std::uint32_t>(i % 100)};
            while (!ring.try_push(tick)) {
                std::this_thread::yield();
            }
        }
        mo::Tick sentinel = {kTickCount, 0.0, 0};
        while (!ring.try_push(sentinel)) {
            std::this_thread::yield();
        }
    });

    bool ordered = true;
    long received = 0;
    mo::Tick tick;
    while (true) {
        if (!ring.try_pop(tick)) {
            std::this_thread::yield();
            continue;
        }
        if (tick.sequence == kTickCount) {
            break;
        }
        if (tick.sequence != static_cast<std::uint64_t>(received)) {
            ordered = false;
        }
        ++received;
    }

    producer.join();
    MO_CHECK(ordered);
    MO_CHECK_EQ(received, static_cast<long>(kTickCount));
}
