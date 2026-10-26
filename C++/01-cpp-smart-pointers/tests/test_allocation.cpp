#include <memory>

#include "spl/allocation_probe.h"
#include "test_framework.h"

SPL_TEST(allocate_shared_combines_object_and_control_block) {
    spl::Tracked<spl::AllocProbeTag>::reset_counters();
    spl::AllocationStats::reset();
    {
        std::shared_ptr<spl::AllocProbe> probe =
            std::allocate_shared<spl::AllocProbe>(
                spl::CountingAllocator<spl::AllocProbe>(), 7);
        SPL_CHECK_EQ(probe->value, 7);
        SPL_CHECK_EQ(spl::AllocationStats::allocations(), 1);
        SPL_CHECK_EQ(spl::Tracked<spl::AllocProbeTag>::alive(), 1);
    }
    SPL_CHECK_EQ(spl::Tracked<spl::AllocProbeTag>::alive(), 0);
    SPL_CHECK_EQ(spl::AllocationStats::outstanding(), 0);
}

SPL_TEST(separate_object_and_control_block_need_two_allocations) {
    spl::AllocationStats::reset();
    {
        std::shared_ptr<spl::AllocProbe> probe(
            new spl::AllocProbe(9),
            std::default_delete<spl::AllocProbe>(),
            spl::CountingAllocator<spl::AllocProbe>());
        SPL_CHECK_EQ(probe->value, 9);
        SPL_CHECK_EQ(spl::AllocationStats::allocations(), 2);
    }
    SPL_CHECK_EQ(spl::AllocationStats::outstanding(), 0);
}

SPL_TEST(weak_ptr_delays_combined_block_deallocation) {
    spl::Tracked<spl::AllocProbeTag>::reset_counters();
    spl::AllocationStats::reset();
    std::weak_ptr<spl::AllocProbe> weak;
    {
        std::shared_ptr<spl::AllocProbe> probe =
            std::allocate_shared<spl::AllocProbe>(
                spl::CountingAllocator<spl::AllocProbe>(), 11);
        weak = probe;
        SPL_CHECK_EQ(probe.use_count(), 1);
    }

    SPL_CHECK_EQ(spl::Tracked<spl::AllocProbeTag>::alive(), 0);
    SPL_CHECK_EQ(spl::AllocationStats::outstanding(), 1);
    SPL_CHECK_EQ(spl::AllocationStats::deallocations(), 0);

    weak.reset();
    SPL_CHECK_EQ(spl::AllocationStats::outstanding(), 0);
}
