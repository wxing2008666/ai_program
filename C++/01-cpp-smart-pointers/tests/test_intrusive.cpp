#include <utility>

#include "spl/gpu_buffer.h"
#include "spl/intrusive_ptr.h"
#include "test_framework.h"

namespace {

class BaseResource : public spl::RefCounted {
public:
    BaseResource() : base_value(7) {}
    virtual ~BaseResource() {}
    int base_value;
};

class DerivedResource : public BaseResource {
public:
    DerivedResource() : derived_value(11) {}
    int derived_value;
};

}

SPL_TEST(intrusive_reference_counting_is_shared_like) {
    spl::Tracked<spl::GpuBufferTag>::reset_counters();
    SPL_CHECK_EQ(spl::Tracked<spl::GpuBufferTag>::alive(), 0);

    spl::intrusive_ptr<spl::GpuBuffer> first = spl::make_gpu_buffer(128, 0);
    SPL_CHECK_EQ(first.use_count(), 1);
    SPL_CHECK_EQ(spl::Tracked<spl::GpuBufferTag>::alive(), 1);

    spl::intrusive_ptr<spl::GpuBuffer> second(first);
    SPL_CHECK_EQ(second.use_count(), 2);
    {
        spl::intrusive_ptr<spl::GpuBuffer> third = second;
        SPL_CHECK_EQ(third.use_count(), 3);
    }
    SPL_CHECK_EQ(second.use_count(), 2);

    second.reset();
    SPL_CHECK_EQ(first.use_count(), 1);

    spl::intrusive_ptr<spl::GpuBuffer> moved(std::move(first));
    SPL_CHECK(!first);
    SPL_CHECK_EQ(moved.use_count(), 1);

    moved.reset();
    SPL_CHECK_EQ(spl::Tracked<spl::GpuBufferTag>::alive(), 0);
}

SPL_TEST(intrusive_detach_and_adopt_matches_c_abi_handoff) {
    spl::Tracked<spl::GpuBufferTag>::reset_counters();
    spl::intrusive_ptr<spl::GpuBuffer> owner = spl::make_gpu_buffer(64, 3);
    spl::GpuBuffer* raw = owner.detach();
    SPL_CHECK(!owner);
    SPL_CHECK(raw != 0);
    SPL_CHECK_EQ(raw->use_count(), 1);
    SPL_CHECK_EQ(spl::Tracked<spl::GpuBufferTag>::alive(), 1);

    spl::intrusive_ptr<spl::GpuBuffer> adopted(raw, false);
    SPL_CHECK_EQ(adopted.use_count(), 1);
    SPL_CHECK_EQ(adopted->device(), 3);

    adopted.reset();
    SPL_CHECK_EQ(spl::Tracked<spl::GpuBufferTag>::alive(), 0);
}

SPL_TEST(intrusive_pointer_conversion_and_casts) {
    spl::intrusive_ptr<DerivedResource> derived = spl::make_intrusive<DerivedResource>();
    SPL_CHECK_EQ(derived.use_count(), 1);

    spl::intrusive_ptr<BaseResource> base = derived;
    SPL_CHECK_EQ(base.use_count(), 2);
    SPL_CHECK_EQ(base->base_value, 7);

    spl::intrusive_ptr<DerivedResource> down =
        spl::dynamic_pointer_cast<DerivedResource>(base);
    SPL_CHECK(down);
    SPL_CHECK_EQ(down->derived_value, 11);
    SPL_CHECK_EQ(base.use_count(), 3);

    down.reset();
    base.reset();
    derived.reset();
    SPL_CHECK(!base);
}
