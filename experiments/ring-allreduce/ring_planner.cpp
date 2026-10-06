// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ring_planner.h"
#include "config.h"
#include "pipe.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>

static double queue_bytes_per_sec(BaseQueue* q) {
    // serviceCapacity(1s) returns the link rate in bits/s (_bitrate).
    return (double)q->serviceCapacity(timeFromSec(1)) / 8.0;
}

static void describe(Route* path, PinnedRoute& out) {
    out.path = path;
    out.propagation = 0;
    out.pipes = 0;
    out.bytes_per_sec = std::numeric_limits<double>::infinity();
    std::ostringstream names;
    for (size_t i = 0; i < path->size(); i++) {
        PacketSink* hop = path->at(i);
        Pipe* pipe = dynamic_cast<Pipe*>(hop);
        if (pipe) {
            out.propagation += pipe->delay();
            out.pipes++;
        }
        BaseQueue* q = dynamic_cast<BaseQueue*>(hop);
        if (q) {
            out.queues.push_back(q);
            double bps = queue_bytes_per_sec(q);
            if (bps < out.bytes_per_sec)
                out.bytes_per_sec = bps;
            if (!names.str().empty())
                names << ",";
            names << q->str();
        }
    }
    out.queues_str = names.str();
}

PinnedRoute pin_host_route(FatTreeTopology* top, uint32_t src, uint32_t dst, int spine, std::string& error) {
    PinnedRoute out;
    out.src = src;
    out.dst = dst;
    out.spine = spine;
    out.path = NULL;
    out.propagation = 0;
    out.bytes_per_sec = 0;
    out.pipes = 0;
    error.clear();

    vector<const Route*>* paths = top->get_bidir_paths(src, dst, false);
    if (paths == NULL || paths->empty()) {
        error = "no route from " + to_string(src) + " to " + to_string(dst);
        return out;
    }

    const Route* chosen = NULL;
    if (spine < 0) {
        if (paths->size() != 1) {
            error = "same-leaf pair " + to_string(src) + "->" + to_string(dst)
                + " has " + to_string(paths->size()) + " paths";
            return out;
        }
        chosen = paths->at(0);
    } else {
        uint32_t tor = top->HOST_POD_SWITCH(src);
        BaseQueue* uplink = top->queues_nlp_nup[tor][spine][0];
        if (uplink == NULL) {
            error = "missing uplink to spine " + to_string(spine);
            return out;
        }
        for (size_t p = 0; p < paths->size(); p++) {
            const Route* candidate = paths->at(p);
            for (size_t i = 0; i < candidate->size(); i++) {
                if (candidate->at(i) == uplink)
                    chosen = candidate;
            }
        }
        if (chosen == NULL) {
            error = "no path " + to_string(src) + "->" + to_string(dst) + " via spine " + to_string(spine);
            return out;
        }
    }

    Route* owned = new Route(*chosen);
    describe(owned, out);
    if (out.queues.empty() || out.pipes == 0 || !(out.bytes_per_sec > 0)) {
        error = "degenerate route " + to_string(src) + "->" + to_string(dst);
        return out;
    }
    return out;
}

std::vector<uint32_t> RankOrderPlanner::ring(const std::vector<uint32_t>& hosts) const {
    std::vector<uint32_t> ordered = hosts;
    std::sort(ordered.begin(), ordered.end());
    return ordered;
}

std::vector<uint32_t> FixedRingPlanner::ring(const std::vector<uint32_t>& hosts) const {
    (void)hosts;
    return _ring;
}

std::vector<uint32_t> TspPlanner::ring(const std::vector<uint32_t>& hosts) const {
    const int n = (int)hosts.size();
    std::vector<uint32_t> empty;
    if (n < 2 || (int)_cost.size() != n)
        return empty;
    const double inf = std::numeric_limits<double>::infinity();
    const int full = 1 << n;
    std::vector<std::vector<double> > dp(full, std::vector<double>(n, inf));
    std::vector<std::vector<int> > parent(full, std::vector<int>(n, -1));
    dp[1][0] = 0;
    for (int mask = 0; mask < full; mask++) {
        for (int last = 0; last < n; last++) {
            if (!(mask & (1 << last)) || !(dp[mask][last] < inf))
                continue;
            for (int nxt = 0; nxt < n; nxt++) {
                if (mask & (1 << nxt))
                    continue;
                int nmask = mask | (1 << nxt);
                double cand = dp[mask][last] + _cost[last][nxt];
                if (cand < dp[nmask][nxt]) {
                    dp[nmask][nxt] = cand;
                    parent[nmask][nxt] = last;
                }
            }
        }
    }
    int end = -1;
    double best = inf;
    int all = full - 1;
    for (int j = 1; j < n; j++) {
        double cand = dp[all][j] + _cost[j][0];
        if (cand < best) {
            best = cand;
            end = j;
        }
    }
    if (end < 0)
        return empty;
    std::vector<int> order(n, -1);
    int mask = all;
    int cur = end;
    for (int pos = n - 1; pos >= 0; pos--) {
        order[pos] = cur;
        int prev = parent[mask][cur];
        mask ^= (1 << cur);
        cur = prev;
    }
    std::vector<uint32_t> result;
    for (int i = 0; i < n; i++) {
        if (order[i] < 0 || order[i] >= n)
            return empty;
        result.push_back(hosts[order[i]]);
    }
    return result;
}

OverlapReport measure_overlap(const std::vector<PinnedRoute>& edges, uint64_t chunk_bytes) {
    OverlapReport report;
    std::map<BaseQueue*, int> logical_edges;
    std::map<BaseQueue*, double> offered_bytes;
    for (size_t e = 0; e < edges.size(); e++) {
        report.static_cost_ps += edges[e].propagation;
        std::set<BaseQueue*> once;
        for (size_t q = 0; q < edges[e].queues.size(); q++) {
            BaseQueue* queue = edges[e].queues[q];
            if (!once.insert(queue).second)
                continue;
            logical_edges[queue] += 1;
            offered_bytes[queue] += (double)chunk_bytes;
        }
    }
    for (std::map<BaseQueue*, int>::iterator it = logical_edges.begin(); it != logical_edges.end(); ++it) {
        if (it->second > report.l_max) {
            report.l_max = it->second;
            report.l_queue = it->first->str();
        }
        double load_s = offered_bytes[it->first] / queue_bytes_per_sec(it->first);
        if (load_s > report.h_max_s) {
            report.h_max_s = load_s;
            report.h_queue = it->first->str();
        }
    }
    return report;
}
