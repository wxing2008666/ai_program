#include <atomic>
#include <memory>
#include <thread>

#include "spl/frame.h"
#include "spl/shared_slot.h"
#include "test_framework.h"

SPL_TEST(shared_slot_atomic_load_store_supports_readers_and_writers) {
    spl::FrameBufferPtr initial = spl::make_frame_buffer(16);
    spl::SharedSlot<spl::FrameBuffer> slot(initial);
    std::atomic<bool> start(false);
    std::atomic<int> errors(0);

    std::thread reader([&slot, &start, &errors]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 20000; ++i) {
            spl::FrameBufferPtr snapshot = slot.load();
            if (!snapshot || snapshot->size() == 0) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::thread writer([&slot, &start]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 20000; ++i) {
            slot.store(spl::make_frame_buffer(16 + static_cast<std::size_t>(i % 8)));
        }
    });

    start.store(true, std::memory_order_release);
    reader.join();
    writer.join();

    SPL_CHECK_EQ(errors.load(std::memory_order_relaxed), 0);
    SPL_CHECK(slot.load());
}
