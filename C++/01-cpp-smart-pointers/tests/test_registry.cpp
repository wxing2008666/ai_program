#include <cstdint>
#include <memory>
#include <string>

#include "spl/plugin_registry.h"
#include "spl/shared_slot.h"
#include "test_framework.h"

namespace {

spl::Frame make_plugin_frame() {
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(4 * 4);
    for (int i = 0; i < 16; ++i) {
        buffer->data()[i] = 10;
    }
    return spl::Frame::wrap(buffer, 4, 4);
}

}

SPL_TEST(plugin_registry_owns_plugins_and_returns_shared_handles) {
    spl::Tracked<spl::PluginTag>::reset_counters();
    spl::PluginRegistry registry;
    std::shared_ptr<spl::Plugin> invert = std::make_shared<spl::InvertPlugin>();
    std::weak_ptr<spl::Plugin> observer = invert;

    registry.install(invert);
    SPL_CHECK_EQ(registry.size(), 1);
    std::shared_ptr<spl::Plugin> found = registry.find("invert");
    SPL_CHECK(found.get() == invert.get());
    SPL_CHECK_EQ(found.use_count(), 3);

    invert.reset();
    SPL_CHECK(!observer.expired());

    found.reset();
    SPL_CHECK(!observer.expired());
    SPL_CHECK_EQ(registry.size(), 1);
    SPL_CHECK(registry.remove("invert"));
    SPL_CHECK(observer.expired());
    SPL_CHECK_EQ(registry.size(), 0);
    SPL_CHECK(!registry.remove("invert"));
}

SPL_TEST(plugin_hot_swap_keeps_in_flight_callers_alive) {
    spl::Tracked<spl::PluginTag>::reset_counters();
    spl::PluginRegistry registry;
    std::shared_ptr<spl::Plugin> original = std::make_shared<spl::InvertPlugin>();
    registry.install(original);
    std::weak_ptr<spl::Plugin> observer = original;
    std::shared_ptr<spl::Plugin> in_flight = registry.find("invert");
    spl::PluginSnapshot old_snapshot = registry.snapshot();

    std::shared_ptr<spl::Plugin> replacement =
        std::make_shared<spl::PassthroughPlugin>();
    registry.install("invert", replacement);

    SPL_CHECK(registry.find("invert").get() == replacement.get());
    SPL_CHECK_EQ(registry.size(), 1);
    SPL_CHECK(!observer.expired());
    SPL_CHECK_EQ(old_snapshot->size(), 1);
    SPL_CHECK_EQ(old_snapshot->find("invert")->second.get(), original.get());

    original.reset();
    SPL_CHECK(!observer.expired());
    in_flight.reset();
    SPL_CHECK(!observer.expired());
    old_snapshot.reset();
    SPL_CHECK(observer.expired());
}

SPL_TEST(plugin_transform_and_passthrough_ownership) {
    spl::Frame input = make_plugin_frame();
    spl::InvertPlugin invert;
    spl::PassthroughPlugin passthrough;

    spl::Frame inverted = invert.apply(input);
    SPL_CHECK_EQ(static_cast<int>(inverted.at(0, 0)), 245);
    SPL_CHECK(inverted.at(0, 0) == 245);
    SPL_CHECK(!inverted.shares_owner_with(input));

    spl::Frame same = passthrough.apply(input);
    SPL_CHECK(same.shares_owner_with(input));
    SPL_CHECK(same.data() == input.data());
}

SPL_TEST(shared_slot_store_load_preserves_old_snapshots) {
    spl::FrameBufferPtr first = spl::make_frame_buffer(16);
    spl::SharedSlot<spl::FrameBuffer> slot(first);
    spl::FrameBufferPtr loaded_first = slot.load();
    SPL_CHECK(loaded_first.get() == first.get());

    spl::FrameBufferPtr second = spl::make_frame_buffer(32);
    slot.store(second);
    spl::FrameBufferPtr loaded_second = slot.load();
    SPL_CHECK(loaded_second.get() == second.get());
    SPL_CHECK_EQ(loaded_second->size(), 32);
    SPL_CHECK_EQ(loaded_first->size(), 16);
}
