#include <cstdint>
#include <memory>
#include <stdexcept>

#include "spl/frame.h"
#include "spl/memory_utils.h"
#include "test_framework.h"

SPL_TEST(frame_slice_is_zero_copy_pointer_arithmetic) {
    spl::Tracked<spl::FrameBufferTag>::reset_counters();
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(16 * 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            buffer->data()[y * 16 + x] = static_cast<std::uint8_t>(y * 16 + x);
        }
    }

    spl::Frame full = spl::Frame::wrap(buffer, 16, 8);
    spl::Frame roi = full.slice(4, 2, 8, 4);

    SPL_CHECK_EQ(roi.width(), 8);
    SPL_CHECK_EQ(roi.height(), 4);
    SPL_CHECK_EQ(roi.stride(), 16);
    SPL_CHECK(roi.data() == full.data() + 2 * 16 + 4);
    SPL_CHECK_EQ(static_cast<int>(roi.at(0, 0)), 36);
    SPL_CHECK_EQ(static_cast<int>(roi.at(7, 3)), 91);
    SPL_CHECK(roi.shares_owner_with(full));
}

SPL_TEST(frame_slice_keeps_buffer_alive_after_owner_reset) {
    spl::Tracked<spl::FrameBufferTag>::reset_counters();
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(16 * 8);
    std::weak_ptr<spl::FrameBuffer> observer = buffer;
    spl::Frame full = spl::Frame::wrap(buffer, 16, 8);
    buffer->data()[0] = 123;
    spl::Frame roi = full.slice(0, 0, 16, 8);

    buffer.reset();
    full = spl::Frame();
    SPL_CHECK(!observer.expired());
    SPL_CHECK_EQ(static_cast<int>(roi.at(0, 0)), 123);
    SPL_CHECK_EQ(roi.owner_use_count(), 1);

    roi = spl::Frame();
    SPL_CHECK(observer.expired());
    SPL_CHECK_EQ(spl::Tracked<spl::FrameBufferTag>::alive(), 0);
}

SPL_TEST(frame_owner_identity_uses_control_block_not_value) {
    spl::Tracked<spl::FrameBufferTag>::reset_counters();
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(8 * 8);
    spl::Frame full = spl::Frame::wrap(buffer, 8, 8);
    spl::Frame roi = full.slice(2, 2, 4, 4);

    spl::Frame::OwnerKey full_key = full.owner_key();
    spl::Frame::OwnerKey roi_key = roi.owner_key();
    SPL_CHECK(spl::same_owner(full_key, roi_key));
    SPL_CHECK(!full_key.owner_before(roi_key));
    SPL_CHECK(!roi_key.owner_before(full_key));

    spl::FrameBufferPtr other_buffer = spl::make_frame_buffer(8 * 8);
    spl::Frame other = spl::Frame::wrap(other_buffer, 8, 8);
    spl::Frame::OwnerKey other_key = other.owner_key();

    SPL_CHECK(full_key == other_key);
    SPL_CHECK(!spl::same_owner(full_key, other_key));
    SPL_CHECK(full_key.owner_before(other_key) ||
              other_key.owner_before(full_key));
}

SPL_TEST(frame_bounds_are_checked) {
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(4 * 4);
    spl::Frame frame = spl::Frame::wrap(buffer, 4, 4);
    SPL_CHECK_THROW(frame.slice(-1, 0, 2, 2), std::out_of_range);
    SPL_CHECK_THROW(frame.slice(0, 0, 5, 2), std::out_of_range);
    SPL_CHECK_THROW(frame.at(4, 0), std::out_of_range);
    SPL_CHECK_THROW(spl::Frame::wrap(buffer, 5, 4), std::invalid_argument);
    SPL_CHECK_THROW(spl::Frame::wrap(spl::FrameBufferPtr(), 4, 4), std::invalid_argument);
}
