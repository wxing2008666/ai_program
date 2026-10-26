#include "risk/risk.hpp"

struct Bad : risk::MaxAmountRule {
    Bad() : risk::MaxAmountRule(1.0) {}
    const char* id() const override { return "bad"; }
};
