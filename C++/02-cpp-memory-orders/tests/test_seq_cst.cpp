#include "test_framework.h"

#include "mo/seq_cst_gate.h"

MO_TEST(seq_cst_gate_never_observes_both_misses) {
    mo::SeqCstGate gate;

    for (int round = 0; round < 100; ++round) {
        mo::SeqCstGate::Observation observation = gate.run();
        MO_CHECK(observation.at_least_one_saw_the_other());
    }
}
