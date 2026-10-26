#ifndef MO_CONFIG_CELL_H
#define MO_CONFIG_CELL_H

#include <atomic>
#include <stdexcept>

namespace mo {

struct Config {
    int max_order;
    int risk_limit_bps;
    bool halt;

    Config() : max_order(0), risk_limit_bps(0), halt(false) {}
};

class ConfigCell {
public:
    static const int kSlotCount = 4;

    ConfigCell() : current_(nullptr) {}

    ConfigCell(const ConfigCell&) = delete;
    ConfigCell& operator=(const ConfigCell&) = delete;

    Config& prepare(int slot) {
        return *slot_pointer(slot);
    }

    void publish(int slot) {
        current_.store(slot_pointer(slot), std::memory_order_release);
    }

    const Config* load_acquire() const {
        return current_.load(std::memory_order_acquire);
    }

    const Config* load_consume() const {
        return current_.load(std::memory_order_consume);
    }

    int risk_limit_acquire() const {
        const Config* config = load_acquire();
        return config ? config->risk_limit_bps : 0;
    }

    int max_order_consume() const {
        const Config* config = load_consume();
        return config ? config->max_order : 0;
    }

private:
    Config* slot_pointer(int slot) {
        if (slot < 0 || slot >= kSlotCount) {
            throw std::out_of_range("ConfigCell slot is outside the array");
        }
        return &slots_[slot];
    }

    Config slots_[kSlotCount];
    std::atomic<const Config*> current_;
};

}

#endif
