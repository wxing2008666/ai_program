#include <memory>
#include <stdexcept>
#include <utility>

#include "spl/resource.h"
#include "test_framework.h"

SPL_TEST(unique_resource_closes_exactly_once) {
    spl::c_resource_reset_stats();
    {
        spl::UniqueResource resource = spl::make_unique_resource(1);
        SPL_CHECK(resource.get() != 0);
        SPL_CHECK_EQ(resource->id, 1);
        SPL_CHECK_EQ(spl::c_resource_open_count(), 1);
    }
    SPL_CHECK_EQ(spl::c_resource_open_count(), 0);
    SPL_CHECK_EQ(spl::c_resource_total_opened(), 1);
    SPL_CHECK_EQ(spl::c_resource_total_closed(), 1);
}

SPL_TEST(unique_resource_move_transfers_ownership) {
    spl::c_resource_reset_stats();
    spl::UniqueResource first = spl::make_unique_resource(2);
    spl::UniqueResource second(std::move(first));
    SPL_CHECK(!first);
    SPL_CHECK(second);
    SPL_CHECK_EQ(second->id, 2);
    SPL_CHECK_EQ(spl::c_resource_open_count(), 1);
    second.reset();
    SPL_CHECK_EQ(spl::c_resource_open_count(), 0);
}

SPL_TEST(unique_resource_can_adopt_c_handles) {
    spl::c_resource_reset_stats();
    spl::CResource* raw = spl::c_resource_open(3);
    {
        spl::UniqueResource owner = spl::adopt_resource(raw);
        SPL_CHECK_EQ(owner->id, 3);
    }
    SPL_CHECK_EQ(spl::c_resource_open_count(), 0);
    SPL_CHECK_EQ(spl::c_resource_total_closed(), 1);
}

SPL_TEST(shared_resource_closes_when_last_owner_leaves) {
    spl::c_resource_reset_stats();
    spl::SharedResource first = spl::make_shared_resource(4);
    spl::SharedResource second = first;
    SPL_CHECK_EQ(first.use_count(), 2);
    SPL_CHECK_EQ(spl::c_resource_open_count(), 1);

    first.reset();
    SPL_CHECK_EQ(spl::c_resource_open_count(), 1);
    second.reset();
    SPL_CHECK_EQ(spl::c_resource_open_count(), 0);
    SPL_CHECK_EQ(spl::c_resource_total_closed(), 1);
}

SPL_TEST(unique_resource_promotes_to_shared_resource) {
    spl::c_resource_reset_stats();
    spl::UniqueResource unique = spl::make_unique_resource(5);
    spl::SharedResource shared = spl::share_resource(std::move(unique));
    SPL_CHECK(!unique);
    SPL_CHECK_EQ(shared.use_count(), 1);
    SPL_CHECK_EQ(shared->id, 5);

    spl::SharedResource copy = shared;
    SPL_CHECK_EQ(copy.use_count(), 2);
    shared.reset();
    SPL_CHECK_EQ(spl::c_resource_open_count(), 1);
    copy.reset();
    SPL_CHECK_EQ(spl::c_resource_open_count(), 0);
}

SPL_TEST(invalid_c_resource_is_rejected) {
    spl::c_resource_reset_stats();
    SPL_CHECK_THROW(spl::make_unique_resource(-1), std::invalid_argument);
    SPL_CHECK_EQ(spl::c_resource_open_count(), 0);
}
