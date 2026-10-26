#include <memory>
#include <utility>

#include "spl/memory_utils.h"
#include "test_framework.h"

SPL_TEST(make_unique_replaces_new_and_delete) {
    std::unique_ptr<int> value = spl::make_unique<int>(42);
    SPL_CHECK(value.get() != 0);
    SPL_CHECK_EQ(*value, 42);

    std::unique_ptr<int> moved = std::move(value);
    SPL_CHECK(value.get() == 0);
    SPL_CHECK_EQ(*moved, 42);
}

SPL_TEST(make_unique_array_value_initializes) {
    std::unique_ptr<int[]> values = spl::make_unique_array<int>(8);
    for (int i = 0; i < 8; ++i) {
        SPL_CHECK_EQ(values[i], 0);
    }
    values[3] = 17;
    SPL_CHECK_EQ(values[3], 17);
}
