#ifndef MO_LIFECYCLE_H
#define MO_LIFECYCLE_H

#include <atomic>

namespace mo {

class Lifecycle {
public:
    enum class State {
        Running,
        Draining,
        Stopped
    };

    Lifecycle() : state_(static_cast<int>(State::Running)) {}

    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

    bool begin_drain() {
        return transition(State::Running, State::Draining);
    }

    bool finish_drain() {
        return transition(State::Draining, State::Stopped);
    }

    State state() const {
        return static_cast<State>(state_.load(std::memory_order_acquire));
    }

private:
    bool transition(State from, State to) {
        int expected = static_cast<int>(from);
        return state_.compare_exchange_strong(
            expected,
            static_cast<int>(to),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    std::atomic<int> state_;
};

}

#endif
