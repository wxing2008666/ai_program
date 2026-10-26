#include "risk/risk.hpp"

struct Other {};

struct Bad : risk::IRule {
    const char* id() const override { return "bad"; }
    risk::Result evaluate(const risk::Context&) const override {
        return risk::Result();
    }
    Other* clone() const override { return nullptr; }
    void accept(risk::RuleVisitor&) const override {}
};
