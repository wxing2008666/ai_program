#include "test_framework.h"

#include <atomic>
#include <thread>
#include <vector>

#include "mo/ref_counted.h"

MO_TEST(ref_counted_destroys_after_last_release) {
    const long before = mo::RefCountedFrame::alive();
    mo::RefCountedFrame* frame = new mo::RefCountedFrame(41);
    MO_CHECK_EQ(frame->use_count(), 1);
    MO_CHECK_EQ(frame->sequence(), static_cast<std::uint64_t>(41));

    frame->retain();
    MO_CHECK_EQ(frame->use_count(), 2);
    frame->release();
    MO_CHECK_EQ(mo::RefCountedFrame::alive(), before + 1);

    frame->release();
    MO_CHECK_EQ(mo::RefCountedFrame::alive(), before);
}

MO_TEST(ref_counted_survives_concurrent_retain_release) {
    const long before = mo::RefCountedFrame::alive();
    mo::RefCountedFrame* frame = new mo::RefCountedFrame(99);
    std::atomic<bool> payload_ok(true);
    std::vector<std::thread> workers;

    for (int i = 0; i < 8; ++i) {
        frame->retain();
        workers.push_back(std::thread([frame, &payload_ok]() {
            if (frame->sequence() != 99 || frame->checksum() == 0) {
                payload_ok.store(false, std::memory_order_relaxed);
            }
            frame->release();
        }));
    }

    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }

    MO_CHECK(payload_ok.load(std::memory_order_relaxed));
    MO_CHECK_EQ(frame->use_count(), 1);
    frame->release();
    MO_CHECK_EQ(mo::RefCountedFrame::alive(), before);
}
