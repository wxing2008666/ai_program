#include "spl/allocation_probe.h"

namespace spl {

std::atomic<long> AllocationStats::allocations_(0);
std::atomic<long> AllocationStats::deallocations_(0);

void AllocationStats::reset() {
    allocations_.store(0, std::memory_order_relaxed);
    deallocations_.store(0, std::memory_order_relaxed);
}

void AllocationStats::note_allocation() {
    allocations_.fetch_add(1, std::memory_order_relaxed);
}

void AllocationStats::note_deallocation() {
    deallocations_.fetch_add(1, std::memory_order_relaxed);
}

long AllocationStats::allocations() {
    return allocations_.load(std::memory_order_relaxed);
}

long AllocationStats::deallocations() {
    return deallocations_.load(std::memory_order_relaxed);
}

long AllocationStats::outstanding() {
    return allocations() - deallocations();
}

}
