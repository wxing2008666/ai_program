#ifndef MO_TICK_H
#define MO_TICK_H

#include <cstdint>

namespace mo {

struct Tick {
    std::uint64_t sequence;
    double price;
    std::uint32_t quantity;
};

}

#endif
