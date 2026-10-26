#ifndef MO_SPSC_RING_H
#define MO_SPSC_RING_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace mo {

template <typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity)
        : capacity_(checked_capacity(capacity)),
          mask_(capacity_ - 1),
          buffer_(new T[capacity_]),
          head_(0),
          tail_(0) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    bool try_push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            return false;
        }
        buffer_[head & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        value = buffer_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const {
        return capacity_;
    }

private:
    static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("SpscRing capacity must be a power of two");
        }
        return capacity;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<std::size_t> head_;
    alignas(64) std::atomic<std::size_t> tail_;
};

}

#endif
