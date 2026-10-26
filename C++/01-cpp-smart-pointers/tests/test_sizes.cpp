#include <cstddef>
#include <memory>

#include "spl/gpu_buffer.h"
#include "spl/intrusive_ptr.h"
#include "test_framework.h"

namespace {

struct StatelessDeleter {
    void operator()(int* pointer) const {
        delete pointer;
    }
};

}

SPL_TEST(smart_pointer_object_sizes) {
    SPL_CHECK_EQ(sizeof(std::unique_ptr<int>), sizeof(void*));
    SPL_CHECK_EQ(sizeof(std::unique_ptr<int, StatelessDeleter>), sizeof(void*));
    SPL_CHECK_EQ(sizeof(std::unique_ptr<int, void (*)(int*)>), 2 * sizeof(void*));
    SPL_CHECK_EQ(sizeof(std::shared_ptr<int>), 2 * sizeof(void*));
    SPL_CHECK_EQ(sizeof(std::weak_ptr<int>), 2 * sizeof(void*));
    SPL_CHECK_EQ(sizeof(spl::intrusive_ptr<spl::GpuBuffer>), sizeof(void*));
}
