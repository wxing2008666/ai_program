#ifndef SPL_ALLOCATION_PROBE_H
#define SPL_ALLOCATION_PROBE_H

#include <atomic>
#include <cstddef>
#include <new>

#include "spl/tracked.h"

namespace spl {

class AllocationStats {
public:
    static void reset();
    static void note_allocation();
    static void note_deallocation();
    static long allocations();
    static long deallocations();
    static long outstanding();

private:
    static std::atomic<long> allocations_;
    static std::atomic<long> deallocations_;
};

template <typename T>
class CountingAllocator {
public:
    typedef T value_type;

    template <typename U>
    struct rebind {
        typedef CountingAllocator<U> other;
    };

    CountingAllocator() {}

    template <typename U>
    CountingAllocator(const CountingAllocator<U>&) {}

    T* allocate(std::size_t count) {
        AllocationStats::note_allocation();
        return static_cast<T*>(::operator new(count * sizeof(T)));
    }

    void deallocate(T* pointer, std::size_t) {
        AllocationStats::note_deallocation();
        ::operator delete(pointer);
    }

    template <typename U>
    bool operator==(const CountingAllocator<U>&) const {
        return true;
    }

    template <typename U>
    bool operator!=(const CountingAllocator<U>&) const {
        return false;
    }
};

struct AllocProbeTag {};

class AllocProbe : public Tracked<AllocProbeTag> {
public:
    AllocProbe() : value(0) {}
    explicit AllocProbe(int initial) : value(initial) {}

    static void* operator new(std::size_t size) {
        AllocationStats::note_allocation();
        return ::operator new(size);
    }

    static void operator delete(void* pointer) noexcept {
        AllocationStats::note_deallocation();
        ::operator delete(pointer);
    }

    int value;
    int payload[7] = {};
};

}

#endif
