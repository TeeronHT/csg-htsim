// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ring_metrics.h"

void RunLongQueueLogger::logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt) {
    Stats& s = stats[&queue];
    if (s.name.empty())
        s.name = queue.str();

    if (ev == PKT_ENQUEUE || ev == PKT_SERVICE) {
        mem_b now = queue.queuesize();
        if (now > s.peak_bytes)
            s.peak_bytes = now;
    }
    if (ev == PKT_SERVICE)
        s.bytes_served += (uint64_t)pkt.size();
}

const RunLongQueueLogger::Stats* RunLongQueueLogger::worst_peak() const {
    const Stats* best = NULL;
    for (std::map<BaseQueue*, Stats>::const_iterator it = stats.begin(); it != stats.end(); ++it) {
        if (best == NULL || it->second.peak_bytes > best->peak_bytes)
            best = &it->second;
    }
    return best;
}
