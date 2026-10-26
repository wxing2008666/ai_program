#ifndef MO_SEQ_CST_GATE_H
#define MO_SEQ_CST_GATE_H

#include <atomic>
#include <thread>

namespace mo {

class SeqCstGate {
public:
    struct Observation {
        bool a_saw_b;
        bool b_saw_a;

        bool at_least_one_saw_the_other() const {
            return a_saw_b || b_saw_a;
        }
    };

    SeqCstGate() : a_(false), b_(false) {}

    SeqCstGate(const SeqCstGate&) = delete;
    SeqCstGate& operator=(const SeqCstGate&) = delete;

    Observation run() {
        a_.store(false, std::memory_order_seq_cst);
        b_.store(false, std::memory_order_seq_cst);

        bool a_saw_b = false;
        bool b_saw_a = false;

        std::thread a([&]() {
            a_.store(true, std::memory_order_seq_cst);
            a_saw_b = b_.load(std::memory_order_seq_cst);
        });
        std::thread b([&]() {
            b_.store(true, std::memory_order_seq_cst);
            b_saw_a = a_.load(std::memory_order_seq_cst);
        });

        a.join();
        b.join();
        return Observation{a_saw_b, b_saw_a};
    }

private:
    alignas(64) std::atomic<bool> a_;
    alignas(64) std::atomic<bool> b_;
};

}

#endif
