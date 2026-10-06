// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
// Dedicated ring-AllReduce driver. This is not main_roce.cpp.
//
// Transport is RoceSrc/RoceSink used as a paced sender: composite queues
// with ECN left at its default (off), no PFC, no PLB, no spraying.
// Routes passed to connect() are the same Route objects scored for c_ij.
//
// The collective is round-synchronous: n concurrent transfers per step,
// one BarrierTrigger per step boundary, 2(n-1) steps. That matches the
// alpha-beta formula. It is not an NCCL pipeline.
#include "ring_runner.h"
#include <iostream>
#include <cstdlib>
#include <cstring>

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --mode p2p|collective --experiment 1|2 --bytes N\n"
              << "       [--n 4] [--pin none|spread|stack|spine0]\n"
              << "       [--queue-bytes N] [--mtu 9000] [--seed 1]\n"
              << "       [--alpha-fit SEC --beta-fit SEC_PER_BYTE]\n";
}

int main(int argc, char** argv) {
    RingRunConfig cfg;
    cfg.experiment = 1;
    cfg.mode = "";
    cfg.pin = "";
    cfg.n = 4;
    cfg.bytes = 0;
    cfg.queue_bytes = 0;
    cfg.alpha_fit_s = 0;
    cfg.beta_fit_s_per_byte = 0;
    cfg.has_fit = false;
    cfg.seed = 1;
    cfg.mtu = 9000;

    bool saw_alpha = false, saw_beta = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            cfg.mode = argv[++i];
        } else if (!strcmp(argv[i], "--experiment") && i + 1 < argc) {
            cfg.experiment = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--bytes") && i + 1 < argc) {
            cfg.bytes = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--n") && i + 1 < argc) {
            cfg.n = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pin") && i + 1 < argc) {
            cfg.pin = argv[++i];
        } else if (!strcmp(argv[i], "--queue-bytes") && i + 1 < argc) {
            cfg.queue_bytes = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--mtu") && i + 1 < argc) {
            cfg.mtu = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            cfg.seed = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--alpha-fit") && i + 1 < argc) {
            cfg.alpha_fit_s = strtod(argv[++i], NULL);
            saw_alpha = true;
        } else if (!strcmp(argv[i], "--beta-fit") && i + 1 < argc) {
            cfg.beta_fit_s_per_byte = strtod(argv[++i], NULL);
            saw_beta = true;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (cfg.mode.empty() || cfg.bytes == 0) {
        usage(argv[0]);
        return 1;
    }
    if (cfg.pin.empty())
        cfg.pin = (cfg.experiment == 2 && cfg.mode == "p2p") ? "spine0" : "none";
    if (cfg.queue_bytes == 0) {
        uint64_t chunk = cfg.bytes / (cfg.mode == "collective" ? cfg.n : 1);
        cfg.queue_bytes = chunk * 8;
        if (cfg.queue_bytes < (uint64_t)cfg.mtu * 128)
            cfg.queue_bytes = (uint64_t)cfg.mtu * 128;
    }
    cfg.has_fit = saw_alpha && saw_beta;
    return run_ring_experiment(cfg);
}
