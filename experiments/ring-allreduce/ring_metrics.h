// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RING_METRICS_H
#define RING_METRICS_H

#include "config.h"
#include "loggertypes.h"
#include "queue.h"
#include <map>
#include <string>

// Run-long peak occupancy and bytes served. Existing queue counters
// (drops, strips, instantaneous queuesize) are left untouched.
class RunLongQueueLogger : public QueueLogger {
public:
    struct Stats {
        mem_b peak_bytes;
        uint64_t bytes_served;
        std::string name;
        Stats() : peak_bytes(0), bytes_served(0) {}
    };

    void logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt);
    const Stats* worst_peak() const;

    std::map<BaseQueue*, Stats> stats;
};

#endif
