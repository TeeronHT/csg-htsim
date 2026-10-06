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
              << " --mode p2p|collective|tsp --experiment 1|2|3 --bytes N\n"
              << "       [--n 4] [--pin none|spread|stack|spine0|srcmod2]\n"
              << "       [--ring 0-1-2-3-4-5] [--src I --dst J]\n"
              << "       [--queue-bytes N] [--mtu 9000] [--seed 1]\n"
              << "       [--alpha-fit SEC --beta-fit SEC_PER_BYTE]\n"
              << "       [--alpha-same SEC --beta-same SEC_PER_BYTE\n"
              << "        --alpha-cross SEC --beta-cross SEC_PER_BYTE]\n";
}

int main(int argc, char** argv) {
    RingRunConfig cfg;
    cfg.experiment = 1;
    cfg.mode = "";
    cfg.pin = "";
    cfg.ring = "";
    cfg.src = -1;
    cfg.dst = -1;
    cfg.n = 4;
    cfg.bytes = 0;
    cfg.queue_bytes = 0;
    cfg.alpha_fit_s = 0;
    cfg.beta_fit_s_per_byte = 0;
    cfg.has_fit = false;
    cfg.alpha_same_s = 0;
    cfg.beta_same_s_per_byte = 0;
    cfg.alpha_cross_s = 0;
    cfg.beta_cross_s_per_byte = 0;
    cfg.has_class_fit = false;
    cfg.seed = 1;
    cfg.mtu = 9000;

    bool saw_alpha = false, saw_beta = false;
    bool saw_same_a = false, saw_same_b = false, saw_cross_a = false, saw_cross_b = false;
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
        } else if (!strcmp(argv[i], "--ring") && i + 1 < argc) {
            cfg.ring = argv[++i];
        } else if (!strcmp(argv[i], "--src") && i + 1 < argc) {
            cfg.src = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dst") && i + 1 < argc) {
            cfg.dst = atoi(argv[++i]);
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
        } else if (!strcmp(argv[i], "--alpha-same") && i + 1 < argc) {
            cfg.alpha_same_s = strtod(argv[++i], NULL);
            saw_same_a = true;
        } else if (!strcmp(argv[i], "--beta-same") && i + 1 < argc) {
            cfg.beta_same_s_per_byte = strtod(argv[++i], NULL);
            saw_same_b = true;
        } else if (!strcmp(argv[i], "--alpha-cross") && i + 1 < argc) {
            cfg.alpha_cross_s = strtod(argv[++i], NULL);
            saw_cross_a = true;
        } else if (!strcmp(argv[i], "--beta-cross") && i + 1 < argc) {
            cfg.beta_cross_s_per_byte = strtod(argv[++i], NULL);
            saw_cross_b = true;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (cfg.mode.empty() || cfg.bytes == 0) {
        usage(argv[0]);
        return 1;
    }
    if (cfg.pin.empty()) {
        if (cfg.experiment == 3)
            cfg.pin = "srcmod2";
        else
            cfg.pin = (cfg.experiment == 2 && cfg.mode == "p2p") ? "spine0" : "none";
    }
    if (cfg.queue_bytes == 0) {
        uint32_t divisor = (cfg.mode == "collective" || cfg.mode == "tsp") ? cfg.n : 1;
        uint64_t chunk = cfg.bytes / divisor;
        cfg.queue_bytes = chunk * 8;
        if (cfg.queue_bytes < (uint64_t)cfg.mtu * 128)
            cfg.queue_bytes = (uint64_t)cfg.mtu * 128;
    }
    cfg.has_fit = saw_alpha && saw_beta;
    cfg.has_class_fit = saw_same_a && saw_same_b && saw_cross_a && saw_cross_b;
    return run_ring_experiment(cfg);
}
