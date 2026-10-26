#ifndef MO_REF_COUNTED_H
#define MO_REF_COUNTED_H

#include <atomic>
#include <cstdint>

namespace mo {

class RefCounted {
public:
    void retain() const {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    void release() const {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    long use_count() const {
        return refs_.load(std::memory_order_relaxed);
    }

protected:
    RefCounted() : refs_(1) {}
    virtual ~RefCounted() {}

private:
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

    mutable std::atomic<long> refs_;
};

class RefCountedFrame : public RefCounted {
public:
    explicit RefCountedFrame(std::uint64_t sequence)
        : sequence_(sequence), checksum_(sequence * 2654435761u) {
        alive_counter().fetch_add(1, std::memory_order_relaxed);
    }

    ~RefCountedFrame() {
        alive_counter().fetch_sub(1, std::memory_order_relaxed);
    }

    std::uint64_t sequence() const {
        return sequence_;
    }

    std::uint64_t checksum() const {
        return checksum_;
    }

    static long alive() {
        return alive_counter().load(std::memory_order_relaxed);
    }

private:
    static std::atomic<long>& alive_counter() {
        static std::atomic<long> value(0);
        return value;
    }

    std::uint64_t sequence_;
    std::uint64_t checksum_;
};

}

#endif
