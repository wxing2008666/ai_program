#ifndef SPL_TRACKED_H
#define SPL_TRACKED_H

#include <atomic>
#include <cstddef>

namespace spl {

template <typename Tag>
class Tracked {
public:
    static std::size_t alive() {
        return alive_.load(std::memory_order_relaxed);
    }

    static std::size_t created() {
        return created_.load(std::memory_order_relaxed);
    }

    static std::size_t destroyed() {
        return destroyed_.load(std::memory_order_relaxed);
    }

    static void reset_counters() {
        alive_.store(0, std::memory_order_relaxed);
        created_.store(0, std::memory_order_relaxed);
        destroyed_.store(0, std::memory_order_relaxed);
    }

protected:
    Tracked() {
        ++alive_;
        ++created_;
    }

    Tracked(const Tracked&) {
        ++alive_;
        ++created_;
    }

    Tracked(Tracked&&) {
        ++alive_;
        ++created_;
    }

    Tracked& operator=(const Tracked&) {
        return *this;
    }

    Tracked& operator=(Tracked&&) {
        return *this;
    }

    ~Tracked() {
        --alive_;
        ++destroyed_;
    }

private:
    static std::atomic<std::size_t> alive_;
    static std::atomic<std::size_t> created_;
    static std::atomic<std::size_t> destroyed_;
};

template <typename Tag>
std::atomic<std::size_t> Tracked<Tag>::alive_(0);

template <typename Tag>
std::atomic<std::size_t> Tracked<Tag>::created_(0);

template <typename Tag>
std::atomic<std::size_t> Tracked<Tag>::destroyed_(0);

}

#endif
