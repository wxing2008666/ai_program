#include "risk/risk.hpp"

struct Bad : risk::RuleBase {
    risk::Result evaluate(const risk::Context&) const override {
        return risk::Result();
    }
    risk::Result doEvaluate(const risk::Context&) const override {
        return risk::Result();
    }
};
