#ifndef SPL_SHARED_SLOT_H
#define SPL_SHARED_SLOT_H

#include <memory>
#include <utility>

namespace spl {

template <typename T>
class SharedSlot {
public:
    SharedSlot() : value_() {}

    explicit SharedSlot(std::shared_ptr<T> initial) : value_(std::move(initial)) {}

    SharedSlot(const SharedSlot&) = delete;
    SharedSlot& operator=(const SharedSlot&) = delete;

    void store(std::shared_ptr<T> value) {
        std::atomic_store(&value_, std::move(value));
    }

    std::shared_ptr<T> load() const {
        return std::atomic_load(&value_);
    }

private:
    mutable std::shared_ptr<T> value_;
};

}

#endif
