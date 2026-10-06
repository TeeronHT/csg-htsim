// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RING_SCHEDULE_H
#define RING_SCHEDULE_H

#include <stdint.h>
#include <vector>

// One unidirectional transfer inside a round-synchronous ring AllReduce.
// This schedule is the one assumed by the homogeneous alpha-beta formula:
// 2(n-1) steps, n concurrent sends of s/n bytes in every step, and the
// next step starts only after every transfer in the current step has
// completed. It is not an NCCL pipeline.
struct ScheduledTransfer {
    int step;
    uint32_t src;
    uint32_t dst;
    uint64_t bytes;
};

// `ring` is the host order without repeating the first host.
// Returns 2(n-1) steps. Fails if s is not divisible by n.
std::vector<ScheduledTransfer> ring_allreduce_schedule(const std::vector<uint32_t>& ring,
                                                       uint64_t message_bytes,
                                                       std::string& error);

#endif
