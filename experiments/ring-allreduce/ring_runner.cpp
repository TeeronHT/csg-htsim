// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ring_runner.h"

#include "compositequeue.h"
#include "config.h"
#include "fat_tree_topology.h"
#include "ring_metrics.h"
#include "ring_planner.h"
#include "ring_schedule.h"
#include "roce.h"
#include "trigger.h"

#include <cmath>
#include <iostream>
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

static int spine_for(const RingRunConfig& cfg, uint32_t src, string& error) {
    if (cfg.experiment == 1) {
        if (cfg.pin != "none") {
            error = "experiment 1 does not pin a spine";
            return -1;
        }
        return -1;
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

int run_ring_experiment(const RingRunConfig& cfg) {
    string error;
    if (cfg.mode != "p2p" && cfg.mode != "collective") {
        cerr << "mode must be p2p or collective\n";
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
    string planner_name = "p2p";
    if (cfg.mode == "p2p") {
        ScheduledTransfer t;
        t.step = 0;
        t.bytes = cfg.bytes;
        if (cfg.experiment == 1) {
            t.src = 0;
            t.dst = 1;
        } else {
            t.src = 0;
            t.dst = 2;
        }
        transfers.push_back(t);
    } else {
        vector<uint32_t> ring;
        if (cfg.experiment == 1) {
            RankOrderPlanner planner;
            ring = planner.ring(hosts);
            planner_name = planner.name();
        } else {
            uint32_t fixed_hosts[] = {0, 2, 1, 3};
            vector<uint32_t> fixed(fixed_hosts, fixed_hosts + 4);
            FixedRingPlanner planner(fixed);
            ring = planner.ring(hosts);
            planner_name = planner.name();
        }
        transfers = ring_allreduce_schedule(ring, cfg.bytes, error);
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
        int spine = spine_for(cfg, transfers[i].src, error);
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
    for (size_t i = 0; i < unique_edges.size(); i++) {
        if (unique_edges[i].pipes != expect_pipes) {
            cerr << "path length mismatch on " << unique_edges[i].src << "->" << unique_edges[i].dst
                 << " pipes " << unique_edges[i].pipes << " expected " << expect_pipes << "\n";
            return 1;
        }
        if (unique_edges[i].propagation != prop0 || unique_edges[i].bytes_per_sec != bps0) {
            cerr << "ring edges do not have identical path characteristics\n";
            return 1;
        }
        if (cfg.experiment == 1 && top->HOST_POD_SWITCH(unique_edges[i].src) != top->HOST_POD_SWITCH(unique_edges[i].dst)) {
            cerr << "experiment 1 edge left the leaf\n";
            return 1;
        }
        if (cfg.experiment == 2 && top->HOST_POD_SWITCH(unique_edges[i].src) == top->HOST_POD_SWITCH(unique_edges[i].dst)) {
            cerr << "experiment 2 edge stayed on one leaf\n";
            return 1;
        }
    }

    OverlapReport overlap = measure_overlap(unique_edges, chunk);
    int expect_l = 1;
    if (cfg.mode == "collective" && cfg.experiment == 2 && cfg.pin == "stack")
        expect_l = 2;
    if (overlap.l_max != expect_l) {
        cerr << "L_max is " << overlap.l_max << ", expected " << expect_l << "\n";
        return 1;
    }
    double h_expect = (double)expect_l * (double)chunk / bps0;
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

    double alpha_th = timeAsSec(prop0);
    double beta_th = 1.0 / bps0;
    double t_sim = timeAsSec(tmax);
    double t_theory = 0;
    double t_fit = 0;
    double slow_th = 0;
    double slow_fit = 0;
    if (cfg.mode == "collective") {
        double steps_d = 2.0 * (double)(cfg.n - 1);
        t_theory = steps_d * alpha_th + steps_d / (double)cfg.n * (double)cfg.bytes * beta_th;
        if (cfg.has_fit) {
            t_fit = steps_d * cfg.alpha_fit_s + steps_d / (double)cfg.n * (double)cfg.bytes * cfg.beta_fit_s_per_byte;
            if (t_fit > 0)
                slow_fit = t_sim / t_fit;
        }
        if (t_theory > 0)
            slow_th = t_sim / t_theory;
    }

    const RunLongQueueLogger::Stats* peak = logger.worst_peak();
    string peak_name = peak ? peak->name : "none";
    mem_b peak_bytes = peak ? peak->peak_bytes : 0;

    ostringstream ring_s;
    if (cfg.mode == "collective") {
        vector<uint32_t> shown;
        if (cfg.experiment == 1)
            shown = hosts;
        else {
            shown.push_back(0); shown.push_back(2); shown.push_back(1); shown.push_back(3);
        }
        for (size_t i = 0; i < shown.size(); i++) {
            if (i)
                ring_s << "-";
            ring_s << shown[i];
        }
        ring_s << "-" << shown[0];
    } else {
        ring_s << transfers[0].src << "-" << transfers[0].dst;
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
         << " ring=" << ring_s.str()
         << " pipes=" << expect_pipes
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
         << " valid=" << (valid ? 1 : 0)
         << "\n";

    cerr << "t_sim_us " << timeAsUs(tmax)
         << " valid " << (valid ? 1 : 0)
         << " L_max " << overlap.l_max
         << " H_max_us " << (overlap.h_max_s * 1e6)
         << " peak_bytes " << peak_bytes
         << " peak " << peak_name << "\n";
    return 0;
}
