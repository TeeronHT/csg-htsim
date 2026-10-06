// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ring_runner.h"

#include "compositequeue.h"
#include "config.h"
#include "fat_tree_topology.h"
#include "pipe.h"
#include "ring_metrics.h"
#include "ring_planner.h"
#include "ring_schedule.h"
#include "roce.h"
#include "trigger.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

using namespace std;

static const double LINK_GBPS = 10.0;
static const uint32_t HOP_US = 1;

static string sci(double x) {
    ostringstream o;
    o.setf(ios::scientific);
    o.precision(16);
    o << x;
    return o.str();
}

static string sanitize(const string& s) {
    string out = s;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == ' ' || out[i] == '\n' || out[i] == '\t')
            out[i] = '_';
    }
    return out;
}

static int spine_for(const RingRunConfig& cfg, FatTreeTopology* top,
                     uint32_t src, uint32_t dst, string& error) {
    if (cfg.experiment == 1) {
        if (cfg.pin != "none") {
            error = "experiment 1 does not pin a spine";
            return -1;
        }
        return -1;
    }
    if (cfg.experiment == 3) {
        // Same rule for every pair: the one same-leaf path, or spine = src % 2.
        if (cfg.pin != "srcmod2") {
            error = "experiment 3 pin must be srcmod2";
            return -1;
        }
        if (top->HOST_POD_SWITCH(src) == top->HOST_POD_SWITCH(dst))
            return -1;
        return (int)(src % 2);
    }
    if (cfg.mode == "p2p") {
        if (cfg.pin != "spine0") {
            error = "experiment 2 point-to-point calibration uses pin=spine0";
            return -1;
        }
        return 0;
    }
    if (cfg.pin == "stack")
        return 0;
    if (cfg.pin == "spread")
        return (int)(src % 2);
    error = "experiment 2 collective pin must be spread or stack";
    return -1;
}

static bool parse_ring(const string& text, uint32_t n, vector<uint32_t>& ring, string& error) {
    ring.clear();
    string cur;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == '-') {
            if (cur.empty()) {
                error = "empty ring hop";
                return false;
            }
            ring.push_back((uint32_t)atoi(cur.c_str()));
            cur.clear();
        } else if (text[i] >= '0' && text[i] <= '9') {
            cur.push_back(text[i]);
        } else {
            error = "ring must be hyphenated host ids";
            return false;
        }
    }
    if (ring.size() != n || ring[0] != 0) {
        error = "ring must be n hosts starting at 0";
        return false;
    }
    vector<int> seen(n, 0);
    for (size_t i = 0; i < ring.size(); i++) {
        if (ring[i] >= n || seen[ring[i]]) {
            error = "ring is not a permutation of the hosts";
            return false;
        }
        seen[ring[i]] = 1;
    }
    return true;
}

static string ring_text(const vector<uint32_t>& ring) {
    ostringstream out;
    for (size_t i = 0; i < ring.size(); i++) {
        if (i)
            out << "-";
        out << ring[i];
    }
    if (!ring.empty())
        out << "-" << ring[0];
    return out.str();
}

static string reverse_ring_text(const vector<uint32_t>& ring) {
    vector<uint32_t> rev;
    if (ring.empty())
        return "";
    rev.push_back(ring[0]);
    for (size_t i = ring.size(); i > 1; i--)
        rev.push_back(ring[i - 1]);
    return ring_text(rev);
}

static void walk_queues(vector< vector< vector<BaseQueue*> > >& vec,
                        RunLongQueueLogger& logger, bool attach,
                        uint64_t& drops, uint64_t& strips, uint64_t& bounces) {
    for (size_t i = 0; i < vec.size(); i++) {
        for (size_t j = 0; j < vec[i].size(); j++) {
            for (size_t k = 0; k < vec[i][j].size(); k++) {
                BaseQueue* q = vec[i][j][k];
                if (!q)
                    continue;
                if (attach) {
                    q->setLogger(&logger);
                    continue;
                }
                Queue* qq = dynamic_cast<Queue*>(q);
                if (qq)
                    drops += (uint64_t)qq->num_drops();
                CompositeQueue* cq = dynamic_cast<CompositeQueue*>(q);
                if (cq) {
                    strips += (uint64_t)cq->num_stripped();
                    bounces += (uint64_t)cq->num_bounced();
                }
            }
        }
    }
}

static void all_queues(FatTreeTopology* top, RunLongQueueLogger& logger, bool attach,
                       uint64_t& drops, uint64_t& strips, uint64_t& bounces) {
    walk_queues(top->queues_ns_nlp, logger, attach, drops, strips, bounces);
    walk_queues(top->queues_nlp_ns, logger, attach, drops, strips, bounces);
    walk_queues(top->queues_nlp_nup, logger, attach, drops, strips, bounces);
    walk_queues(top->queues_nup_nlp, logger, attach, drops, strips, bounces);
}

static FatTreeTopology* build_topology(const RingRunConfig& cfg, EventList& eventlist, string& error) {
    uint32_t nodes;
    int tor_down, tor_up, agg_down;
    if (cfg.experiment == 1) {
        if (cfg.n < 2) {
            error = "experiment 1 needs n >= 2";
            return NULL;
        }
        nodes = cfg.n;
        tor_down = (int)cfg.n;
        tor_up = (int)cfg.n;
        agg_down = 1;
    } else if (cfg.experiment == 2) {
        if (cfg.n != 4) {
            error = "experiment 2 is the fixed 4-host two-leaf topology";
            return NULL;
        }
        nodes = 4;
        tor_down = 2;
        tor_up = 2;
        agg_down = 2;
    } else if (cfg.experiment == 3) {
        if (cfg.n != 6) {
            error = "experiment 3 is the 6-host three-leaf topology";
            return NULL;
        }
        // Three leaves of two hosts, two spines. Six uplinks and radix_down 3
        // on the spine tier give two aggregation switches.
        nodes = 6;
        tor_down = 2;
        tor_up = 2;
        agg_down = 3;
    } else {
        error = "experiment must be 1 or 2";
        return NULL;
    }

    linkspeed_bps speed = speedFromGbps(LINK_GBPS);
    mem_b qbytes = (mem_b)cfg.queue_bytes;
    FatTreeTopology::set_tiers(2);
    FatTreeTopology::set_podsize(nodes);
    FatTreeTopology::set_latencies(timeFromUs(HOP_US), timeFromUs(HOP_US), timeFromUs(HOP_US), 0, 0, 0);
    FatTreeTopology::set_tier_parameters(0, tor_up, tor_down, qbytes, qbytes, 1, speed, 1);
    FatTreeTopology::set_tier_parameters(1, 0, agg_down, qbytes, qbytes, 1, speed, 1);
    return new FatTreeTopology(nodes, 0, 0, NULL, &eventlist, NULL, COMPOSITE, FAIR_PRIO, 0);
}

struct LiveFlow {
    RoceSrc* src;
    RoceSink* sink;
    int step;
};

static int run_tsp(const RingRunConfig& cfg) {
    string error;
    if (cfg.experiment != 3 || cfg.n != 6) {
        cerr << "tsp mode is the experiment 3 cost matrix\n";
        return 1;
    }
    EventList eventlist;
    srand(cfg.seed);
    Packet::set_packet_size(cfg.mtu);
    FatTreeTopology* top = build_topology(cfg, eventlist, error);
    if (!top) {
        cerr << error << "\n";
        return 1;
    }
    vector<uint32_t> hosts;
    vector<vector<double> > cost(cfg.n, vector<double>(cfg.n, 0));
    for (uint32_t h = 0; h < cfg.n; h++)
        hosts.push_back(h);
    for (uint32_t src = 0; src < cfg.n; src++) {
        for (uint32_t dst = 0; dst < cfg.n; dst++) {
            if (src == dst)
                continue;
            int spine = spine_for(cfg, top, src, dst, error);
            if (!error.empty()) {
                cerr << error << "\n";
                return 1;
            }
            PinnedRoute route = pin_host_route(top, src, dst, spine, error);
            if (!error.empty()) {
                cerr << error << "\n";
                return 1;
            }
            cost[src][dst] = (double)route.propagation;
        }
    }
    TspPlanner planner(cost);
    vector<uint32_t> ring = planner.ring(hosts);
    if (ring.size() != cfg.n) {
        cerr << "tsp planner returned no ring\n";
        return 1;
    }
    uint64_t static_cost = 0;
    for (size_t i = 0; i < ring.size(); i++)
        static_cost += (uint64_t)cost[ring[i]][ring[(i + 1) % ring.size()]];
    cout << "RECORD mode=tsp experiment=3 pin=" << cfg.pin
         << " planner=tsp_static_cost ring=" << ring_text(ring)
         << " reverse=" << reverse_ring_text(ring)
         << " static_cost_ps=" << static_cost
         << " seed=" << cfg.seed << "\n";
    return 0;
}

struct DirectedLink {
    BaseQueue* queue;
    Pipe* pipe;
};

static DirectedLink make_link(EventList& eventlist, linkspeed_bps speed, mem_b depth,
                              simtime_picosec delay, const string& name, bool host_uplink) {
    DirectedLink link;
    if (host_uplink) {
        FairPriorityQueue* q = new FairPriorityQueue(speed, memFromPkt(1000), eventlist, NULL);
        q->forceName(name);
        link.queue = q;
    } else {
        CompositeQueue* q = new CompositeQueue(speed, depth, eventlist, NULL);
        q->forceName(name);
        link.queue = q;
    }
    link.pipe = new Pipe(delay, eventlist);
    link.pipe->forceName("Pipe-" + name);
    return link;
}

// Two sites of three hosts, joined by one fiber. Local delivery stays on the
// site switch. Inter-site delivery uses the border and that fiber, and the
// border does not hairpin back to the same site.
struct TwoSiteNetwork {
    DirectedLink up_local[6];
    DirectedLink down_local[6];
    DirectedLink up_border[6];
    DirectedLink down_border[6];
    DirectedLink fiber_lr;
    DirectedLink fiber_rl;
    vector<BaseQueue*> queues;
};

static bool same_site(uint32_t a, uint32_t b) {
    return (a < 3) == (b < 3);
}

static TwoSiteNetwork* build_twosite(EventList& eventlist, mem_b depth) {
    linkspeed_bps speed = speedFromGbps(LINK_GBPS);
    TwoSiteNetwork* net = new TwoSiteNetwork();
    for (uint32_t h = 0; h < 6; h++) {
        string site = (h < 3) ? "ML" : "MR";
        string border = (h < 3) ? "BL" : "BR";
        net->up_local[h] = make_link(eventlist, speed, depth, timeFromUs((uint32_t)2),
                                      "SRC" + to_string(h) + "->" + site, true);
        net->down_local[h] = make_link(eventlist, speed, depth, timeFromUs((uint32_t)2),
                                        site + "->DST" + to_string(h), false);
        net->up_border[h] = make_link(eventlist, speed, depth, timeFromUs((uint32_t)1),
                                       "SRC" + to_string(h) + "->" + border, true);
        net->down_border[h] = make_link(eventlist, speed, depth, timeFromUs((uint32_t)1),
                                         border + "->DST" + to_string(h), false);
    }
    net->fiber_lr = make_link(eventlist, speed, depth, timeFromUs((uint32_t)1), "BL->BR", false);
    net->fiber_rl = make_link(eventlist, speed, depth, timeFromUs((uint32_t)1), "BR->BL", false);
    for (uint32_t h = 0; h < 6; h++) {
        net->queues.push_back(net->up_local[h].queue);
        net->queues.push_back(net->down_local[h].queue);
        net->queues.push_back(net->up_border[h].queue);
        net->queues.push_back(net->down_border[h].queue);
    }
    net->queues.push_back(net->fiber_lr.queue);
    net->queues.push_back(net->fiber_rl.queue);
    return net;
}

static void push_link(Route* path, PinnedRoute& out, const DirectedLink& link) {
    path->push_back(link.queue);
    path->push_back(link.pipe);
    out.queues.push_back(link.queue);
    out.propagation += link.pipe->delay();
    out.pipes++;
    double bps = (double)link.queue->serviceCapacity(timeFromSec(1)) / 8.0;
    if (bps < out.bytes_per_sec)
        out.bytes_per_sec = bps;
    if (!out.queues_str.empty())
        out.queues_str += ",";
    out.queues_str += link.queue->str();
}

static PinnedRoute pin_twosite(TwoSiteNetwork* net, uint32_t src, uint32_t dst, string& error) {
    PinnedRoute out;
    out.src = src;
    out.dst = dst;
    out.spine = -1;
    out.path = new Route();
    out.propagation = 0;
    out.bytes_per_sec = numeric_limits<double>::infinity();
    out.pipes = 0;
    error.clear();
    if (src > 5 || dst > 5 || src == dst) {
        error = "two-site endpoints must be distinct hosts in 0..5";
        return out;
    }
    if (same_site(src, dst)) {
        push_link(out.path, out, net->up_local[src]);
        push_link(out.path, out, net->down_local[dst]);
    } else if (src < 3) {
        push_link(out.path, out, net->up_border[src]);
        push_link(out.path, out, net->fiber_lr);
        push_link(out.path, out, net->down_border[dst]);
    } else {
        push_link(out.path, out, net->up_border[src]);
        push_link(out.path, out, net->fiber_rl);
        push_link(out.path, out, net->down_border[dst]);
    }
    return out;
}

static void tally_queues(const vector<BaseQueue*>& queues, RunLongQueueLogger& logger, bool attach,
                         uint64_t& drops, uint64_t& strips, uint64_t& bounces) {
    for (size_t i = 0; i < queues.size(); i++) {
        BaseQueue* q = queues[i];
        if (attach) {
            q->setLogger(&logger);
            continue;
        }
        Queue* qq = dynamic_cast<Queue*>(q);
        if (qq)
            drops += (uint64_t)qq->num_drops();
        CompositeQueue* cq = dynamic_cast<CompositeQueue*>(q);
        if (cq) {
            strips += (uint64_t)cq->num_stripped();
            bounces += (uint64_t)cq->num_bounced();
        }
    }
}

static int run_twosite(const RingRunConfig& cfg) {
    string error;
    if (cfg.n != 6) {
        cerr << "experiment 3b is the 6-host two-site topology\n";
        return 1;
    }
    if (cfg.mode != "p2p" && cfg.mode != "collective" && cfg.mode != "tsp") {
        cerr << "mode must be p2p, collective, or tsp\n";
        return 1;
    }
    if (cfg.bytes == 0 || cfg.mtu <= 0 || (cfg.bytes % (uint64_t)cfg.mtu) != 0) {
        cerr << "bytes must be a positive multiple of the MSS\n";
        return 1;
    }

    EventList eventlist;
    srand(cfg.seed);
    Packet::set_packet_size(cfg.mtu);
    TwoSiteNetwork* net = build_twosite(eventlist, (mem_b)cfg.queue_bytes);

    if (cfg.mode == "tsp") {
        vector<uint32_t> hosts;
        vector<vector<double> > cost(6, vector<double>(6, 0));
        for (uint32_t h = 0; h < 6; h++)
            hosts.push_back(h);
        for (uint32_t src = 0; src < 6; src++) {
            for (uint32_t dst = 0; dst < 6; dst++) {
                if (src == dst)
                    continue;
                PinnedRoute route = pin_twosite(net, src, dst, error);
                if (!error.empty()) {
                    cerr << error << "\n";
                    return 1;
                }
                cost[src][dst] = (double)route.propagation;
            }
        }
        TspPlanner planner(cost);
        vector<uint32_t> chosen = planner.ring(hosts);
        if (chosen.size() != 6) {
            cerr << "tsp planner returned no ring\n";
            return 1;
        }
        uint64_t static_cost = 0;
        for (size_t i = 0; i < chosen.size(); i++)
            static_cost += (uint64_t)cost[chosen[i]][chosen[(i + 1) % chosen.size()]];
        cout << "RECORD mode=tsp experiment=3b pin=shortest planner=tsp_static_cost"
             << " ring=" << ring_text(chosen)
             << " reverse=" << reverse_ring_text(chosen)
             << " static_cost_ps=" << static_cost
             << " seed=" << cfg.seed << "\n";
        return 0;
    }

    vector<ScheduledTransfer> transfers;
    vector<uint32_t> ring_order;
    string planner_name = "p2p";
    if (cfg.mode == "p2p") {
        if (cfg.src < 0 || cfg.dst < 0) {
            cerr << "experiment 3b point-to-point needs --src and --dst\n";
            return 1;
        }
        ScheduledTransfer t;
        t.step = 0;
        t.src = (uint32_t)cfg.src;
        t.dst = (uint32_t)cfg.dst;
        t.bytes = cfg.bytes;
        transfers.push_back(t);
    } else {
        if (!parse_ring(cfg.ring, cfg.n, ring_order, error)) {
            cerr << error << "\n";
            return 1;
        }
        planner_name = "fixed";
        transfers = ring_allreduce_schedule(ring_order, cfg.bytes, error);
        if (!error.empty()) {
            cerr << error << "\n";
            return 1;
        }
    }

    uint64_t chunk = transfers[0].bytes;
    if (chunk % (uint64_t)cfg.mtu != 0) {
        cerr << "s/n is not a multiple of the MSS\n";
        return 1;
    }

    map<pair<uint32_t, uint32_t>, PinnedRoute> pinned;
    vector<PinnedRoute> unique_edges;
    int inter_site = 0;
    for (size_t i = 0; i < transfers.size(); i++) {
        pair<uint32_t, uint32_t> key(transfers[i].src, transfers[i].dst);
        if (pinned.find(key) != pinned.end())
            continue;
        PinnedRoute route = pin_twosite(net, transfers[i].src, transfers[i].dst, error);
        if (!error.empty()) {
            cerr << error << "\n";
            return 1;
        }
        bool same = same_site(route.src, route.dst);
        simtime_picosec want_prop = same ? timeFromUs((uint32_t)4) : timeFromUs((uint32_t)3);
        int want_pipes = same ? 2 : 3;
        BaseQueue* fiber = (route.src < 3) ? net->fiber_lr.queue : net->fiber_rl.queue;
        bool uses_fiber = false;
        for (size_t q = 0; q < route.queues.size(); q++) {
            if (route.queues[q] == fiber)
                uses_fiber = true;
        }
        if (route.pipes != want_pipes || route.propagation != want_prop || uses_fiber == same) {
            cerr << "two-site route " << route.src << "->" << route.dst
                 << " pipes " << route.pipes << " prop_us " << timeAsUs(route.propagation) << "\n";
            return 1;
        }
        if (!same)
            inter_site++;
        pinned[key] = route;
        unique_edges.push_back(route);
        cerr << "edge " << route.src << "->" << route.dst
             << " pipes " << route.pipes
             << " prop_us " << timeAsUs(route.propagation)
             << " queues " << route.queues_str << "\n";
    }

    double bps0 = unique_edges[0].bytes_per_sec;
    simtime_picosec max_prop = 0;
    for (size_t i = 0; i < unique_edges.size(); i++) {
        if (unique_edges[i].bytes_per_sec != bps0) {
            cerr << "two-site edges do not share a link rate\n";
            return 1;
        }
        if (unique_edges[i].propagation > max_prop)
            max_prop = unique_edges[i].propagation;
    }
    OverlapReport overlap = measure_overlap(unique_edges, chunk);
    double h_expect = (double)overlap.l_max * (double)chunk / bps0;
    if (!(h_expect > 0) || fabs(overlap.h_max_s - h_expect) / h_expect > 1e-6) {
        cerr << "H_max " << overlap.h_max_s << " does not match L_max * (s/n) / C = " << h_expect << "\n";
        return 1;
    }

    int steps = (cfg.mode == "collective") ? (int)(2 * (cfg.n - 1)) : 1;
    vector<BarrierTrigger*> barriers;
    if (cfg.mode == "collective") {
        for (int s = 0; s < steps - 1; s++)
            barriers.push_back(new BarrierTrigger(eventlist, (triggerid_t)(s + 1), cfg.n));
    }

    linkspeed_bps nic = speedFromGbps(LINK_GBPS);
    vector<LiveFlow> flows;
    for (size_t i = 0; i < transfers.size(); i++) {
        const ScheduledTransfer& t = transfers[i];
        Route* fwd_path = pinned[make_pair(t.src, t.dst)].path;
        pair<uint32_t, uint32_t> back_key(t.dst, t.src);
        if (pinned.find(back_key) == pinned.end()) {
            PinnedRoute back = pin_twosite(net, t.dst, t.src, error);
            if (!error.empty()) {
                cerr << "reverse route: " << error << "\n";
                return 1;
            }
            pinned[back_key] = back;
        }
        Route* rev_path = pinned[back_key].path;

        RoceSrc* src = new RoceSrc(NULL, NULL, eventlist, nic);
        RoceSink* snk = new RoceSink();
        src->set_flowsize(t.bytes);
        src->set_dst(t.dst);
        snk->set_src(t.src);
        src->setName("roce_s" + to_string(t.step) + "_" + to_string(t.src) + "_" + to_string(t.dst));
        snk->setName("roce_sink_s" + to_string(t.step) + "_" + to_string(t.src) + "_" + to_string(t.dst));

        HostQueue* hostq = dynamic_cast<HostQueue*>(fwd_path->at(0));
        if (!hostq) {
            cerr << "host uplink is not a HostQueue\n";
            return 1;
        }
        hostq->addHostSender(src);

        Route* data = new Route(*fwd_path, *snk);
        Route* ack = new Route(*rev_path, *src);
        check_non_null(data);
        check_non_null(ack);

        simtime_picosec start = 0;
        if (cfg.mode == "collective" && t.step > 0)
            start = TRIGGER_START;
        src->connect(data, ack, *snk, start);
        if (cfg.mode == "collective" && t.step > 0)
            barriers[t.step - 1]->add_target(*src);
        if (cfg.mode == "collective" && t.step + 1 < steps)
            src->set_end_trigger(*barriers[t.step]);

        LiveFlow live;
        live.src = src;
        live.sink = snk;
        live.step = t.step;
        flows.push_back(live);
    }

    RunLongQueueLogger logger;
    uint64_t drops = 0, strips = 0, bounces = 0;
    tally_queues(net->queues, logger, true, drops, strips, bounces);
    cerr << "schedule round_synchronous_not_nccl_pipeline planner " << planner_name
         << " flows " << flows.size() << " steps " << steps << "\n";
    EventList::setEndtime(timeFromSec(2));
    while (EventList::doNextEvent()) {
    }
    drops = strips = bounces = 0;
    tally_queues(net->queues, logger, false, drops, strips, bounces);

    uint64_t rtx = 0, nacks = 0, pauses = 0, gaps = 0, finished = 0;
    simtime_picosec tmax = 0;
    for (size_t i = 0; i < flows.size(); i++) {
        RoceSrc* src = flows[i].src;
        rtx += src->_rtx_packets_sent;
        nacks += src->_nacks_received;
        pauses += src->_pauses;
        gaps += flows[i].sink->_drops;
        if (src->_flow_started && src->_completion_time > 0) {
            finished++;
            if (src->_completion_time > tmax)
                tmax = src->_completion_time;
        }
    }
    bool valid = finished == flows.size()
        && drops == 0 && strips == 0 && bounces == 0
        && rtx == 0 && nacks == 0 && pauses == 0 && gaps == 0
        && tmax > 0;

    double beta_th = 1.0 / bps0;
    double t_sim = timeAsSec(tmax);
    double t_theory = 0;
    double t_fit = 0;
    double slow_th = 0;
    double slow_fit = 0;
    if (cfg.mode == "collective") {
        double steps_d = 2.0 * (double)(cfg.n - 1);
        double slowest = 0;
        for (size_t i = 0; i < unique_edges.size(); i++) {
            double step = timeAsSec(unique_edges[i].propagation) + (double)chunk / unique_edges[i].bytes_per_sec;
            if (step > slowest)
                slowest = step;
        }
        t_theory = steps_d * slowest;
        if (cfg.has_class_fit) {
            double slowest_fit = 0;
            for (size_t i = 0; i < unique_edges.size(); i++) {
                bool same = unique_edges[i].pipes == 2;
                double step = (same ? cfg.alpha_same_s : cfg.alpha_cross_s)
                    + (double)chunk * (same ? cfg.beta_same_s_per_byte : cfg.beta_cross_s_per_byte);
                if (step > slowest_fit)
                    slowest_fit = step;
            }
            t_fit = steps_d * slowest_fit;
        }
        if (t_fit > 0)
            slow_fit = t_sim / t_fit;
        if (t_theory > 0)
            slow_th = t_sim / t_theory;
    }

    mem_b fiber_peak = 0;
    const RunLongQueueLogger::Stats* lr = NULL;
    const RunLongQueueLogger::Stats* rl = NULL;
    map<BaseQueue*, RunLongQueueLogger::Stats>::const_iterator it;
    it = logger.stats.find(net->fiber_lr.queue);
    if (it != logger.stats.end())
        lr = &it->second;
    it = logger.stats.find(net->fiber_rl.queue);
    if (it != logger.stats.end())
        rl = &it->second;
    if (lr)
        fiber_peak = lr->peak_bytes;
    if (rl && rl->peak_bytes > fiber_peak)
        fiber_peak = rl->peak_bytes;

    const RunLongQueueLogger::Stats* peak = logger.worst_peak();
    string peak_name = peak ? peak->name : "none";
    mem_b peak_bytes = peak ? peak->peak_bytes : 0;
    string ring_s = (cfg.mode == "collective") ? ring_text(ring_order)
        : (to_string(transfers[0].src) + "-" + to_string(transfers[0].dst));
    string reverse_s = (cfg.mode == "collective") ? reverse_ring_text(ring_order) : "na";

    cout << "RECORD"
         << " mode=" << cfg.mode
         << " experiment=3b"
         << " pin=shortest"
         << " planner=" << planner_name
         << " transport=roce"
         << " queue=composite"
         << " ecn=off pfc=off plb=off spraying=off route=pinned"
         << " schedule=" << (cfg.mode == "collective" ? "round_synchronous_not_nccl_pipeline" : "single_flow")
         << " n=" << cfg.n
         << " bytes=" << cfg.bytes
         << " chunk=" << chunk
         << " mtu=" << cfg.mtu
         << " link_gbps=" << LINK_GBPS
         << " queue_bytes=" << cfg.queue_bytes
         << " seed=" << cfg.seed
         << " ring=" << ring_s
         << " reverse=" << reverse_s
         << " inter_site=" << inter_site
         << " alpha_theory_s=" << sci(timeAsSec(max_prop))
         << " beta_theory_s_per_byte=" << sci(beta_th)
         << " alpha_same_s=" << (cfg.has_class_fit ? sci(cfg.alpha_same_s) : string("na"))
         << " beta_same_s_per_byte=" << (cfg.has_class_fit ? sci(cfg.beta_same_s_per_byte) : string("na"))
         << " alpha_cross_s=" << (cfg.has_class_fit ? sci(cfg.alpha_cross_s) : string("na"))
         << " beta_cross_s_per_byte=" << (cfg.has_class_fit ? sci(cfg.beta_cross_s_per_byte) : string("na"))
         << " t_theory_s=" << sci(t_theory)
         << " t_fit_s=" << sci(t_fit)
         << " t_sim_s=" << sci(t_sim)
         << " slowdown_theory=" << sci(slow_th)
         << " slowdown_fit=" << sci(slow_fit)
         << " l_max=" << overlap.l_max
         << " h_max_s=" << sci(overlap.h_max_s)
         << " l_queue=" << sanitize(overlap.l_queue)
         << " h_queue=" << sanitize(overlap.h_queue)
         << " static_cost_ps=" << overlap.static_cost_ps
         << " peak_queue_bytes=" << peak_bytes
         << " peak_queue=" << sanitize(peak_name)
         << " fiber_peak_bytes=" << fiber_peak
         << " drops=" << drops
         << " strips=" << strips
         << " bounces=" << bounces
         << " rtx=" << rtx
         << " nacks=" << nacks
         << " pauses=" << pauses
         << " sink_gaps=" << gaps
         << " flows_done=" << finished
         << " flows_expected=" << flows.size()
         << " valid=" << (valid ? 1 : 0)
         << "\n";
    cerr << "t_sim_us " << timeAsUs(tmax)
         << " valid " << (valid ? 1 : 0)
         << " L_max " << overlap.l_max
         << " inter_site " << inter_site
         << " fiber_peak " << fiber_peak << "\n";
    return 0;
}

int run_ring_experiment(const RingRunConfig& cfg) {
    if (cfg.experiment == 4)
        return run_twosite(cfg);
    string error;
    if (cfg.mode == "tsp")
        return run_tsp(cfg);
    if (cfg.mode != "p2p" && cfg.mode != "collective") {
        cerr << "mode must be p2p, collective, or tsp\n";
        return 1;
    }
    if (cfg.bytes == 0 || cfg.mtu <= 0 || (cfg.bytes % (uint64_t)cfg.mtu) != 0) {
        cerr << "bytes must be a positive multiple of the MSS\n";
        return 1;
    }
    if (cfg.queue_bytes < (uint64_t)cfg.mtu * 64) {
        cerr << "queue is smaller than 64 packets\n";
        return 1;
    }

    EventList eventlist;
    srand(cfg.seed);
    Packet::set_packet_size(cfg.mtu);

    FatTreeTopology* top = build_topology(cfg, eventlist, error);
    if (!top) {
        cerr << error << "\n";
        return 1;
    }

    vector<uint32_t> hosts;
    for (uint32_t h = 0; h < cfg.n; h++)
        hosts.push_back(h);

    vector<ScheduledTransfer> transfers;
    vector<uint32_t> ring_order;
    string planner_name = "p2p";
    if (cfg.mode == "p2p") {
        ScheduledTransfer t;
        t.step = 0;
        t.bytes = cfg.bytes;
        if (cfg.experiment == 1) {
            t.src = 0;
            t.dst = 1;
        } else if (cfg.experiment == 3) {
            if (cfg.src < 0 || cfg.dst < 0) {
                cerr << "experiment 3 point-to-point needs --src and --dst\n";
                return 1;
            }
            t.src = (uint32_t)cfg.src;
            t.dst = (uint32_t)cfg.dst;
        } else {
            t.src = 0;
            t.dst = 2;
        }
        transfers.push_back(t);
    } else {
        if (cfg.experiment == 1) {
            RankOrderPlanner planner;
            ring_order = planner.ring(hosts);
            planner_name = planner.name();
        } else if (cfg.experiment == 3) {
            if (!parse_ring(cfg.ring, cfg.n, ring_order, error)) {
                cerr << error << "\n";
                return 1;
            }
            planner_name = "fixed";
        } else {
            uint32_t fixed_hosts[] = {0, 2, 1, 3};
            ring_order.assign(fixed_hosts, fixed_hosts + 4);
            planner_name = "fixed";
        }
        transfers = ring_allreduce_schedule(ring_order, cfg.bytes, error);
        if (!error.empty()) {
            cerr << error << "\n";
            return 1;
        }
    }

    uint64_t chunk = transfers[0].bytes;
    if (chunk % (uint64_t)cfg.mtu != 0) {
        cerr << "s/n is not a multiple of the MSS\n";
        return 1;
    }

    map<pair<uint32_t, uint32_t>, PinnedRoute> pinned;
    vector<PinnedRoute> unique_edges;
    for (size_t i = 0; i < transfers.size(); i++) {
        pair<uint32_t, uint32_t> key(transfers[i].src, transfers[i].dst);
        if (pinned.find(key) != pinned.end())
            continue;
        int spine = spine_for(cfg, top, transfers[i].src, transfers[i].dst, error);
        if (!error.empty()) {
            cerr << error << "\n";
            return 1;
        }
        PinnedRoute route = pin_host_route(top, transfers[i].src, transfers[i].dst, spine, error);
        if (!error.empty()) {
            cerr << error << "\n";
            return 1;
        }
        pinned[key] = route;
        unique_edges.push_back(route);
        cerr << "edge " << route.src << "->" << route.dst
             << " spine " << route.spine
             << " pipes " << route.pipes
             << " prop_us " << timeAsUs(route.propagation)
             << " queues " << route.queues_str << "\n";
    }

    int expect_pipes = (cfg.experiment == 1) ? 2 : 4;
    simtime_picosec prop0 = unique_edges[0].propagation;
    double bps0 = unique_edges[0].bytes_per_sec;
    int shortcuts = 0;
    simtime_picosec max_prop = 0;
    for (size_t i = 0; i < unique_edges.size(); i++) {
        const PinnedRoute& edge = unique_edges[i];
        bool same = top->HOST_POD_SWITCH(edge.src) == top->HOST_POD_SWITCH(edge.dst);
        if (edge.propagation > max_prop)
            max_prop = edge.propagation;
        if (same)
            shortcuts++;
        if (cfg.experiment == 3) {
            int want_pipes = same ? 2 : 4;
            int want_spine = same ? -1 : (int)(edge.src % 2);
            if (edge.pipes != want_pipes || edge.spine != want_spine) {
                cerr << "experiment 3 route " << edge.src << "->" << edge.dst
                     << " pipes " << edge.pipes << " spine " << edge.spine << "\n";
                return 1;
            }
            if (edge.bytes_per_sec != bps0) {
                cerr << "experiment 3 edges do not share a link rate\n";
                return 1;
            }
        } else {
            if (edge.pipes != expect_pipes) {
                cerr << "path length mismatch on " << edge.src << "->" << edge.dst
                     << " pipes " << edge.pipes << " expected " << expect_pipes << "\n";
                return 1;
            }
            if (edge.propagation != prop0 || edge.bytes_per_sec != bps0) {
                cerr << "ring edges do not have identical path characteristics\n";
                return 1;
            }
            if (cfg.experiment == 1 && !same) {
                cerr << "experiment 1 edge left the leaf\n";
                return 1;
            }
            if (cfg.experiment == 2 && same) {
                cerr << "experiment 2 edge stayed on one leaf\n";
                return 1;
            }
        }
    }

    OverlapReport overlap = measure_overlap(unique_edges, chunk);
    int expect_l = 1;
    if (cfg.mode == "collective" && cfg.experiment == 2 && cfg.pin == "stack")
        expect_l = 2;
    if (cfg.experiment != 3 && overlap.l_max != expect_l) {
        cerr << "L_max is " << overlap.l_max << ", expected " << expect_l << "\n";
        return 1;
    }
    double h_denom = (cfg.experiment == 3) ? (double)overlap.l_max : (double)expect_l;
    double h_expect = h_denom * (double)chunk / bps0;
    if (!(h_expect > 0) || fabs(overlap.h_max_s - h_expect) / h_expect > 1e-6) {
        cerr << "H_max " << overlap.h_max_s << " does not match L_max * (s/n) / C = " << h_expect << "\n";
        return 1;
    }

    int steps = 1;
    if (cfg.mode == "collective")
        steps = (int)(2 * (cfg.n - 1));
    vector<BarrierTrigger*> barriers;
    if (cfg.mode == "collective") {
        for (int s = 0; s < steps - 1; s++)
            barriers.push_back(new BarrierTrigger(eventlist, (triggerid_t)(s + 1), cfg.n));
    }

    linkspeed_bps nic = speedFromGbps(LINK_GBPS);
    vector<LiveFlow> flows;
    for (size_t i = 0; i < transfers.size(); i++) {
        const ScheduledTransfer& t = transfers[i];
        // Copy the pointers out before any further map insert. Inserting the
        // reverse route can rehash and invalidate references into `pinned`.
        Route* fwd_path = pinned[make_pair(t.src, t.dst)].path;
        int spine = pinned[make_pair(t.src, t.dst)].spine;
        pair<uint32_t, uint32_t> back_key(t.dst, t.src);
        if (pinned.find(back_key) == pinned.end()) {
            PinnedRoute back = pin_host_route(top, t.dst, t.src, spine, error);
            if (!error.empty()) {
                cerr << "reverse route: " << error << "\n";
                return 1;
            }
            // The ACK path is explicit. Overlap is computed on data paths only.
            pinned[back_key] = back;
        }
        Route* rev_path = pinned[back_key].path;

        RoceSrc* src = new RoceSrc(NULL, NULL, eventlist, nic);
        RoceSink* snk = new RoceSink();
        src->set_flowsize(t.bytes);
        src->set_dst(t.dst);
        snk->set_src(t.src);
        src->setName("roce_s" + to_string(t.step) + "_" + to_string(t.src) + "_" + to_string(t.dst));
        snk->setName("roce_sink_s" + to_string(t.step) + "_" + to_string(t.src) + "_" + to_string(t.dst));

        BaseQueue* hq = top->queues_ns_nlp[t.src][top->HOST_POD_SWITCH(t.src)][0];
        HostQueue* hostq = dynamic_cast<HostQueue*>(hq);
        if (!hostq) {
            cerr << "host uplink is not a HostQueue\n";
            return 1;
        }
        hostq->addHostSender(src);

        Route* data = new Route(*fwd_path, *snk);
        Route* ack = new Route(*rev_path, *src);
        check_non_null(data);
        check_non_null(ack);

        simtime_picosec start = 0;
        if (cfg.mode == "collective" && t.step > 0)
            start = TRIGGER_START;
        src->connect(data, ack, *snk, start);
        if (cfg.mode == "collective" && t.step > 0)
            barriers[t.step - 1]->add_target(*src);
        if (cfg.mode == "collective" && t.step + 1 < steps)
            src->set_end_trigger(*barriers[t.step]);

        LiveFlow live;
        live.src = src;
        live.sink = snk;
        live.step = t.step;
        flows.push_back(live);
    }

    RunLongQueueLogger logger;
    uint64_t drops = 0, strips = 0, bounces = 0;
    all_queues(top, logger, true, drops, strips, bounces);

    cerr << "schedule round_synchronous_not_nccl_pipeline planner " << planner_name
         << " flows " << flows.size() << " steps " << steps << "\n";
    EventList::setEndtime(timeFromSec(2));
    while (EventList::doNextEvent()) {
    }

    drops = strips = bounces = 0;
    all_queues(top, logger, false, drops, strips, bounces);

    uint64_t rtx = 0, nacks = 0, pauses = 0, gaps = 0, finished = 0;
    simtime_picosec tmax = 0;
    for (size_t i = 0; i < flows.size(); i++) {
        RoceSrc* src = flows[i].src;
        rtx += src->_rtx_packets_sent;
        nacks += src->_nacks_received;
        pauses += src->_pauses;
        gaps += flows[i].sink->_drops;
        if (src->_flow_started && src->_completion_time > 0) {
            finished++;
            if (src->_completion_time > tmax)
                tmax = src->_completion_time;
        }
    }

    bool valid = finished == flows.size()
        && drops == 0 && strips == 0 && bounces == 0
        && rtx == 0 && nacks == 0 && pauses == 0 && gaps == 0
        && tmax > 0;

    double alpha_th = timeAsSec(cfg.experiment == 3 ? max_prop : prop0);
    double beta_th = 1.0 / bps0;
    double t_sim = timeAsSec(tmax);
    double t_theory = 0;
    double t_fit = 0;
    double slow_th = 0;
    double slow_fit = 0;
    if (cfg.mode == "collective") {
        double steps_d = 2.0 * (double)(cfg.n - 1);
        if (cfg.experiment == 3) {
            // Every step runs the same n edges. The barrier waits for the slowest one.
            double slowest = 0;
            for (size_t i = 0; i < unique_edges.size(); i++) {
                double step = timeAsSec(unique_edges[i].propagation)
                    + (double)chunk / unique_edges[i].bytes_per_sec;
                if (step > slowest)
                    slowest = step;
            }
            t_theory = steps_d * slowest;
            if (cfg.has_class_fit) {
                double slowest_fit = 0;
                for (size_t i = 0; i < unique_edges.size(); i++) {
                    bool same = unique_edges[i].pipes == 2;
                    double step = (same ? cfg.alpha_same_s : cfg.alpha_cross_s)
                        + (double)chunk * (same ? cfg.beta_same_s_per_byte : cfg.beta_cross_s_per_byte);
                    if (step > slowest_fit)
                        slowest_fit = step;
                }
                t_fit = steps_d * slowest_fit;
            }
        } else {
            t_theory = steps_d * alpha_th + steps_d / (double)cfg.n * (double)cfg.bytes * beta_th;
            if (cfg.has_fit)
                t_fit = steps_d * cfg.alpha_fit_s + steps_d / (double)cfg.n * (double)cfg.bytes * cfg.beta_fit_s_per_byte;
        }
        if (t_fit > 0)
            slow_fit = t_sim / t_fit;
        if (t_theory > 0)
            slow_th = t_sim / t_theory;
    }

    const RunLongQueueLogger::Stats* peak = logger.worst_peak();
    string peak_name = peak ? peak->name : "none";
    mem_b peak_bytes = peak ? peak->peak_bytes : 0;

    string ring_s;
    string reverse_s = "na";
    if (cfg.mode == "collective") {
        ring_s = ring_text(ring_order);
        if (cfg.experiment == 3)
            reverse_s = reverse_ring_text(ring_order);
    } else {
        ring_s = to_string(transfers[0].src) + "-" + to_string(transfers[0].dst);
    }

    cout << "RECORD"
         << " mode=" << cfg.mode
         << " experiment=" << cfg.experiment
         << " pin=" << cfg.pin
         << " planner=" << planner_name
         << " transport=roce"
         << " queue=composite"
         << " ecn=off"
         << " pfc=off"
         << " plb=off"
         << " spraying=off"
         << " route=pinned"
         << " schedule=" << (cfg.mode == "collective" ? "round_synchronous_not_nccl_pipeline" : "single_flow")
         << " n=" << cfg.n
         << " bytes=" << cfg.bytes
         << " chunk=" << chunk
         << " mtu=" << cfg.mtu
         << " link_gbps=" << LINK_GBPS
         << " hop_us=" << HOP_US
         << " queue_bytes=" << cfg.queue_bytes
         << " seed=" << cfg.seed
         << " ring=" << ring_s
         << " pipes=" << (cfg.experiment == 3 ? string("mixed") : to_string(expect_pipes))
         << " alpha_theory_s=" << sci(alpha_th)
         << " beta_theory_s_per_byte=" << sci(beta_th)
         << " alpha_fit_s=" << (cfg.has_fit ? sci(cfg.alpha_fit_s) : string("na"))
         << " beta_fit_s_per_byte=" << (cfg.has_fit ? sci(cfg.beta_fit_s_per_byte) : string("na"))
         << " t_theory_s=" << sci(t_theory)
         << " t_fit_s=" << sci(t_fit)
         << " t_sim_s=" << sci(t_sim)
         << " slowdown_theory=" << sci(slow_th)
         << " slowdown_fit=" << sci(slow_fit)
         << " l_max=" << overlap.l_max
         << " h_max_s=" << sci(overlap.h_max_s)
         << " l_queue=" << sanitize(overlap.l_queue)
         << " h_queue=" << sanitize(overlap.h_queue)
         << " static_cost_ps=" << overlap.static_cost_ps
         << " peak_queue_bytes=" << peak_bytes
         << " peak_queue=" << sanitize(peak_name)
         << " drops=" << drops
         << " strips=" << strips
         << " bounces=" << bounces
         << " rtx=" << rtx
         << " nacks=" << nacks
         << " pauses=" << pauses
         << " sink_gaps=" << gaps
         << " flows_done=" << finished
         << " flows_expected=" << flows.size()
         << " valid=" << (valid ? 1 : 0);
    if (cfg.experiment == 3) {
        cout << " shortcuts=" << shortcuts
             << " reverse=" << reverse_s
             << " alpha_same_s=" << (cfg.has_class_fit ? sci(cfg.alpha_same_s) : string("na"))
             << " beta_same_s_per_byte=" << (cfg.has_class_fit ? sci(cfg.beta_same_s_per_byte) : string("na"))
             << " alpha_cross_s=" << (cfg.has_class_fit ? sci(cfg.alpha_cross_s) : string("na"))
             << " beta_cross_s_per_byte=" << (cfg.has_class_fit ? sci(cfg.beta_cross_s_per_byte) : string("na"));
    }
    cout << "\n";

    cerr << "t_sim_us " << timeAsUs(tmax)
         << " valid " << (valid ? 1 : 0)
         << " L_max " << overlap.l_max
         << " H_max_us " << (overlap.h_max_s * 1e6)
         << " peak_bytes " << peak_bytes
         << " peak " << peak_name << "\n";
    return 0;
}
