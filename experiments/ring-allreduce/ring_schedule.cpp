// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ring_schedule.h"

std::vector<ScheduledTransfer> ring_allreduce_schedule(const std::vector<uint32_t>& ring,
                                                       uint64_t message_bytes,
                                                       std::string& error) {
    std::vector<ScheduledTransfer> out;
    error.clear();
    if (ring.size() < 2) {
        error = "ring needs at least 2 hosts";
        return out;
    }
    uint32_t n = (uint32_t)ring.size();
    if (message_bytes == 0 || message_bytes % n != 0) {
        error = "message size must be a positive multiple of the ring length";
        return out;
    }
    uint64_t chunk = message_bytes / n;
    int steps = (int)(2 * (n - 1));
    for (int step = 0; step < steps; step++) {
        for (uint32_t i = 0; i < n; i++) {
            ScheduledTransfer t;
            t.step = step;
            t.src = ring[i];
            t.dst = ring[(i + 1) % n];
            t.bytes = chunk;
            out.push_back(t);
        }
    }
    return out;
}
