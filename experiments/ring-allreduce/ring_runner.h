// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RING_RUNNER_H
#define RING_RUNNER_H

#include <stdint.h>
#include <string>

// One process runs one simulation. EventList and the fat-tree static
// parameters are process-wide, so calibration and each experiment are
// separate invocations of this function.
struct RingRunConfig {
    int experiment;          // 1: one leaf. 2: two leaves. 3: three-leaf fat tree. 4: two-site fiber.
    std::string mode;        // "p2p", "collective", or "tsp"
    std::string pin;         // "none", "spread", "stack", "spine0", "srcmod2"
    std::string ring;        // hyphenated host order for experiment 3, starting at 0
    int src;                 // experiment 3 point-to-point endpoints; -1 otherwise
    int dst;
    uint32_t n;
    uint64_t bytes;
    uint64_t queue_bytes;
    double alpha_fit_s;     // meaningful only when has_fit is set
    double beta_fit_s_per_byte;
    bool has_fit;
    // Experiment 3 has two route classes. The step prediction is the
    // slowest pinned edge, not one alpha for the whole ring.
    double alpha_same_s;
    double beta_same_s_per_byte;
    double alpha_cross_s;
    double beta_cross_s_per_byte;
    bool has_class_fit;
    unsigned seed;
    int mtu;
};

// Prints one "RECORD ..." line on success. Returns 0 on a completed
// run (including a run that is invalid because of loss). Returns 1
// when the topology or the schedule could not be constructed.
int run_ring_experiment(const RingRunConfig& cfg);

#endif
