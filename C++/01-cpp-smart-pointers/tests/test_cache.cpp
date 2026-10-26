#include <memory>
#include <string>

#include "spl/frame_cache.h"
#include "spl/memory_utils.h"
#include "test_framework.h"

SPL_TEST(frame_cache_does_not_extend_buffer_lifetime) {
    spl::Tracked<spl::FrameBufferTag>::reset_counters();
    spl::FrameCache cache;
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(64);
    std::weak_ptr<spl::FrameBuffer> observer = buffer;

    cache.put(7, buffer);
    SPL_CHECK_EQ(cache.size(), 1);
    spl::FrameBufferPtr hit = cache.get(7);
    SPL_CHECK(hit.get() == buffer.get());
    SPL_CHECK_EQ(hit.use_count(), 2);

    hit.reset();
    buffer.reset();
    SPL_CHECK(observer.expired());
    SPL_CHECK_EQ(cache.size(), 1);

    SPL_CHECK(!cache.get(7));
    SPL_CHECK_EQ(cache.size(), 0);
    SPL_CHECK_EQ(spl::Tracked<spl::FrameBufferTag>::alive(), 0);
}

SPL_TEST(frame_cache_purges_expired_entries) {
    spl::FrameCache cache;
    spl::FrameBufferPtr first = spl::make_frame_buffer(32);
    spl::FrameBufferPtr second = spl::make_frame_buffer(32);
    cache.put(1, first);
    cache.put(2, second);
    SPL_CHECK_EQ(cache.size(), 2);

    first.reset();
    second.reset();
    SPL_CHECK_EQ(cache.purge_expired(), 2);
    SPL_CHECK_EQ(cache.size(), 0);
}

SPL_TEST(owner_registry_keys_by_control_block) {
    spl::Tracked<spl::FrameBufferTag>::reset_counters();
    spl::OwnerRegistry registry;
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(8 * 8);
    spl::Frame full = spl::Frame::wrap(buffer, 8, 8);
    spl::Frame roi = full.slice(1, 1, 4, 4);

    registry.annotate(full.owner_key(), "camera-0");
    SPL_CHECK_EQ(registry.size(), 1);
    SPL_CHECK_EQ(registry.label_of(roi.owner_key()), std::string("camera-0"));

    std::weak_ptr<spl::FrameBuffer> observer = buffer;
    buffer.reset();
    full = spl::Frame();
    roi = spl::Frame();
    SPL_CHECK(!observer.expired());
    SPL_CHECK_EQ(spl::Tracked<spl::FrameBufferTag>::alive(), 1);

    registry.clear();
    SPL_CHECK(observer.expired());
    SPL_CHECK_EQ(spl::Tracked<spl::FrameBufferTag>::alive(), 0);
}

SPL_TEST(owner_registry_distinguishes_null_alias_keys) {
    spl::OwnerRegistry registry;
    spl::FrameBufferPtr first_buffer = spl::make_frame_buffer(16);
    spl::FrameBufferPtr second_buffer = spl::make_frame_buffer(16);
    spl::Frame first = spl::Frame::wrap(first_buffer, 4, 4);
    spl::Frame second = spl::Frame::wrap(second_buffer, 4, 4);

    registry.annotate(first.owner_key(), "first");
    registry.annotate(second.owner_key(), "second");
    SPL_CHECK_EQ(registry.size(), 2);
    SPL_CHECK_EQ(registry.label_of(first.owner_key()), std::string("first"));
    SPL_CHECK_EQ(registry.label_of(second.owner_key()), std::string("second"));

    SPL_CHECK_EQ(registry.erase(first.owner_key()), true);
    SPL_CHECK_EQ(registry.size(), 1);
    SPL_CHECK_EQ(registry.label_of(first.owner_key()), std::string(""));
}
