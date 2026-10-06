// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RING_PLANNER_H
#define RING_PLANNER_H

#include "fat_tree_topology.h"
#include "queue.h"
#include "route.h"
#include <stdint.h>
#include <string>
#include <vector>

// A pinned host-to-host route. The Route* is the exact object later
// wrapped with the flow's sink; htsim does not choose another path.
struct PinnedRoute {
    uint32_t src;
    uint32_t dst;
    int spine; // -1 when the path stays on one leaf
    Route* path;
    simtime_picosec propagation;
    double bytes_per_sec;
    int pipes;
    std::vector<BaseQueue*> queues;
    std::string queues_str;
};

class RingPlanner {
public:
    virtual ~RingPlanner() {}
    // Host order around the ring, without repeating the first host.
    virtual std::vector<uint32_t> ring(const std::vector<uint32_t>& hosts) const = 0;
    virtual const char* name() const = 0;
};

class RankOrderPlanner : public RingPlanner {
public:
    std::vector<uint32_t> ring(const std::vector<uint32_t>& hosts) const;
    const char* name() const { return "rank_order"; }
};

class FixedRingPlanner : public RingPlanner {
public:
    explicit FixedRingPlanner(const std::vector<uint32_t>& ring) : _ring(ring) {}
    std::vector<uint32_t> ring(const std::vector<uint32_t>& hosts) const;
    const char* name() const { return "fixed"; }
private:
    std::vector<uint32_t> _ring;
};

// Minimum-cost Hamiltonian cycle on a dense cost matrix indexed in the
// same order as `hosts`. Not used by Experiments 1 or 2.
class TspPlanner : public RingPlanner {
public:
    explicit TspPlanner(const std::vector<std::vector<double> >& cost) : _cost(cost) {}
    std::vector<uint32_t> ring(const std::vector<uint32_t>& hosts) const;
    const char* name() const { return "tsp_static_cost"; }
private:
    std::vector<std::vector<double> > _cost;
};

// spine < 0 selects the single same-leaf path. Otherwise the path must
// traverse that aggregation switch.
PinnedRoute pin_host_route(FatTreeTopology* top, uint32_t src, uint32_t dst, int spine, std::string& error);

struct OverlapReport {
    int l_max;
    double h_max_s;
    std::string l_queue;
    std::string h_queue;
    uint64_t static_cost_ps;
    OverlapReport() : l_max(0), h_max_s(0), static_cost_ps(0) {}
};

// chunk_bytes is d_r, the data offered by one logical edge during one step.
OverlapReport measure_overlap(const std::vector<PinnedRoute>& edges, uint64_t chunk_bytes);

#endif
