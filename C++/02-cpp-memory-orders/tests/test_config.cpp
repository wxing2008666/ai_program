#include "test_framework.h"

#include "mo/config_cell.h"

MO_TEST(config_cell_publishes_immutable_snapshot) {
    mo::ConfigCell cell;
    MO_CHECK(cell.load_acquire() == nullptr);
    MO_CHECK_EQ(cell.risk_limit_acquire(), 0);
    MO_CHECK_EQ(cell.max_order_consume(), 0);

    mo::Config& first = cell.prepare(0);
    first.max_order = 10;
    first.risk_limit_bps = 250;
    first.halt = false;
    cell.publish(0);

    const mo::Config* acquired = cell.load_acquire();
    MO_CHECK(acquired != nullptr);
    MO_CHECK_EQ(acquired->max_order, 10);
    MO_CHECK_EQ(acquired->risk_limit_bps, 250);
    MO_CHECK(!acquired->halt);
    MO_CHECK_EQ(cell.risk_limit_acquire(), 250);
    MO_CHECK_EQ(cell.max_order_consume(), 10);

    mo::Config& second = cell.prepare(1);
    second.max_order = 20;
    second.risk_limit_bps = 500;
    second.halt = true;
    cell.publish(1);

    MO_CHECK_EQ(cell.risk_limit_acquire(), 500);
    MO_CHECK_EQ(cell.max_order_consume(), 20);
    MO_CHECK(cell.load_acquire()->halt);
}

MO_TEST(config_cell_rejects_invalid_slots) {
    mo::ConfigCell cell;
    MO_CHECK_THROW(cell.prepare(-1));
    MO_CHECK_THROW(cell.prepare(mo::ConfigCell::kSlotCount));
}
