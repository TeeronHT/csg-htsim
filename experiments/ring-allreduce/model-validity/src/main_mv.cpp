// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
// One dumbbell simulation: N paced RoCE flows share one core link.
// This sender is fixed-rate RoCE. It is not DCQCN.
#include "config.h"
#include "loggertypes.h"
#include "compositequeue.h"
#include "pipe.h"
#include "eventlist.h"
#include "network.h"
#include "roce.h"
#include "route.h"
#include "ecn.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

struct FlowSpec {
    int src;
    int dst;
    int core;
    uint64_t nominal;
    uint64_t wire;
    double rate_gbps;
    double start_us;
};

struct MvConfig {
    string run_id;
    int senders;
    uint64_t nominal_bytes;
    uint64_t wire_bytes;
    int mss;
    double sender_rate_gbps;
    double sender_access_gbps;
    double receiver_access_gbps;
    double bottleneck_gbps;
    double propagation_us;
    uint64_t queue_bytes;
    unsigned seed;
    double endtime_s;
    string trace;       // none, summary, full
    string trace_path;
    vector<double> starts_us;
    vector<FlowSpec> flows;
};

struct Hop {
    string name;
    BaseQueue* queue;
    Pipe* pipe;
    double capacity_gbps;
    bool bottleneck;
};

struct QStat {
    string name;
    mem_b peak;
    uint64_t bytes_served;
    uint64_t ecn_marks;
    double integral_byte_ps;
    simtime_picosec last_t;
    mem_b last_q;
    bool tracking;
    QStat() : peak(0), bytes_served(0), ecn_marks(0), integral_byte_ps(0),
              last_t(0), last_q(0), tracking(false) {}
};

struct SvcRec {
    simtime_picosec t;
    uint32_t flow;
    int core;
    uint64_t bytes;
};

class MvQueueLogger : public QueueLogger {
public:
    BaseQueue* bottleneck;
    bool full_trace;
    ofstream* trace;
    map<BaseQueue*, QStat> stats;
    map<BaseQueue*, int> core_index;
    vector<SvcRec> services;

    MvQueueLogger() : bottleneck(NULL), full_trace(false), trace(NULL) {}

    void logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt) {
        QStat& s = stats[&queue];
        if (s.name.empty())
            s.name = queue.str();
        simtime_picosec now = EventList::now();
        if (!s.tracking) {
            s.tracking = true;
            s.last_t = 0;
            s.last_q = 0;
        }
        if (now >= s.last_t)
            s.integral_byte_ps += (double)s.last_q * (double)(now - s.last_t);
        s.last_t = now;
        s.last_q = queue.queuesize();
        if (ev == PKT_ENQUEUE || ev == PKT_SERVICE) {
            if (s.last_q > s.peak)
                s.peak = s.last_q;
        }
        if (ev == PKT_SERVICE) {
            s.bytes_served += (uint64_t)pkt.size();
            if (pkt.flags() & ECN_CE)
                s.ecn_marks++;
            map<BaseQueue*, int>::iterator cit = core_index.find(&queue);
            if (cit != core_index.end()) {
                SvcRec rec;
                rec.t = now;
                rec.flow = pkt.flow_id();
                rec.core = cit->second;
                rec.bytes = (uint64_t)pkt.size();
                services.push_back(rec);
            }
        }
        if (full_trace && trace && &queue == bottleneck &&
            (ev == PKT_ENQUEUE || ev == PKT_SERVICE)) {
            *trace << timeAsUs(now) << "," << s.last_q << "\n";
        }
    }

    void close_integral(BaseQueue* queue, simtime_picosec tmax) {
        map<BaseQueue*, QStat>::iterator it = stats.find(queue);
        if (it == stats.end())
            return;
        QStat& s = it->second;
        if (!s.tracking) {
            s.tracking = true;
            s.last_t = 0;
            s.last_q = 0;
        }
        if (tmax > s.last_t)
            s.integral_byte_ps += (double)s.last_q * (double)(tmax - s.last_t);
        s.last_t = tmax;
    }
};

static void usage() {
    cerr << "Usage: htsim_mv --senders N --wire-bytes N --nominal-bytes N\n"
         << "       --sender-rate-gbps R --queue-bytes N [--run-id ID]\n"
         << "       [--mss 9000] [--seed 1] [--propagation-us 1]\n"
         << "       [--sender-access-gbps 10] [--receiver-access-gbps 10]\n"
         << "       [--bottleneck-gbps 10] [--endtime-s 10]\n"
         << "       [--trace none|summary|full] [--trace-path FILE]\n"
         << "       [--starts-us t0,t1,...]  one start per sender, microseconds\n"
         << "       [--flow src,dst,nominal,wire,rate_gbps,start_us,core]\n";
}

static bool take(int& i, int argc, char** argv, const char* flag, string& out) {
    if (strcmp(argv[i], flag) != 0 || i + 1 >= argc)
        return false;
    out = argv[++i];
    return true;
}

static string sci(double x) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.16e", x);
    return buf;
}

static int run_scheduled(MvConfig& cfg);

static string sanitize(const string& s) {
    string o;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == ' ' || c == '\n' || c == '=')
            o.push_back('_');
        else
            o.push_back(c);
    }
    return o.empty() ? string("none") : o;
}

static Hop make_access(const string& name, bool fair, double gbps, mem_b depth,
                       EventList& eventlist, simtime_picosec hop, vector<BaseQueue*>& queues) {
    BaseQueue* q;
    if (fair) {
        FairPriorityQueue* up = new FairPriorityQueue(speedFromGbps(gbps), depth, eventlist, NULL);
        up->forceName(name);
        q = up;
    } else {
        CompositeQueue* down = new CompositeQueue(speedFromGbps(gbps), depth, eventlist, NULL);
        down->forceName(name);
        q = down;
    }
    Hop h;
    h.name = q->str();
    h.queue = q;
    h.pipe = new Pipe(hop, eventlist);
    h.pipe->setName(string("Pipe-") + h.name);
    h.capacity_gbps = gbps;
    h.bottleneck = false;
    queues.push_back(q);
    return h;
}

// Each --flow is one paced transfer. Repeating them on one sender is how an
// ON/OFF background is built: a burst is a real flow, and the OFF gap is the
// time until the next one starts.
static int run_scheduled(MvConfig& cfg) {
    if (cfg.flows.empty() || cfg.mss <= 0 || cfg.queue_bytes == 0 || cfg.bottleneck_gbps <= 0 ||
        cfg.sender_access_gbps <= 0 || cfg.receiver_access_gbps <= 0) {
        usage();
        return 1;
    }
    int ncores = 1;
    for (size_t i = 0; i < cfg.flows.size(); i++) {
        FlowSpec& f = cfg.flows[i];
        if ((f.wire % (uint64_t)cfg.mss) != 0) {
            cerr << "flow wire bytes are not a multiple of mss\n";
            return 1;
        }
        if (f.core + 1 > ncores)
            ncores = f.core + 1;
    }

    EventList eventlist;
    srand(cfg.seed);
    Packet::set_packet_size(cfg.mss);
    EventList::setEndtime(timeFromSec(cfg.endtime_s));
    simtime_picosec hop = timeFromSec(cfg.propagation_us * 1e-6);
    mem_b access_depth = memFromPkt(1000);
    vector<BaseQueue*> queues;
    vector<Hop> report;

    map<int, Hop> left_up, left_down, right_up, right_down;
    for (size_t i = 0; i < cfg.flows.size(); i++) {
        int src = cfg.flows[i].src;
        int dst = cfg.flows[i].dst;
        if (left_up.find(src) == left_up.end()) {
            left_up[src] = make_access("SRC" + to_string(src) + "->L", true,
                                       cfg.sender_access_gbps, access_depth, eventlist, hop, queues);
            left_down[src] = make_access("L->SRC" + to_string(src), false,
                                         cfg.sender_access_gbps, access_depth, eventlist, hop, queues);
            report.push_back(left_up[src]);
            report.push_back(left_down[src]);
        }
        if (right_up.find(dst) == right_up.end()) {
            right_up[dst] = make_access("SRC" + to_string(dst) + "->R", true,
                                        cfg.receiver_access_gbps, access_depth, eventlist, hop, queues);
            right_down[dst] = make_access("R->DST" + to_string(dst), false,
                                          cfg.receiver_access_gbps, access_depth, eventlist, hop, queues);
            report.push_back(right_up[dst]);
            report.push_back(right_down[dst]);
        }
        if (left_up.count(dst) || right_up.count(src)) {
            cerr << "a host is used on both sides of the dumbbell\n";
            return 1;
        }
    }

    struct Core {
        Hop lr;
        Hop rl;
    };
    vector<Core> cores(ncores);
    for (int c = 0; c < ncores; c++) {
        string tag = (c == 0) ? string("") : ("#" + to_string(c));
        CompositeQueue* lr = new CompositeQueue(speedFromGbps(cfg.bottleneck_gbps),
                                                 (mem_b)cfg.queue_bytes, eventlist, NULL);
        lr->forceName("L->R" + tag);
        cores[c].lr.queue = lr;
        cores[c].lr.name = lr->str();
        cores[c].lr.pipe = new Pipe(hop, eventlist);
        cores[c].lr.pipe->setName("Pipe-L->R" + tag);
        cores[c].lr.capacity_gbps = cfg.bottleneck_gbps;
        cores[c].lr.bottleneck = true;
        queues.push_back(lr);

        CompositeQueue* rl = new CompositeQueue(speedFromGbps(cfg.bottleneck_gbps),
                                                 access_depth, eventlist, NULL);
        rl->forceName("R->L" + tag);
        cores[c].rl.queue = rl;
        cores[c].rl.name = rl->str();
        cores[c].rl.pipe = new Pipe(hop, eventlist);
        cores[c].rl.pipe->setName("Pipe-R->L" + tag);
        cores[c].rl.capacity_gbps = cfg.bottleneck_gbps;
        cores[c].rl.bottleneck = false;
        queues.push_back(rl);
        report.push_back(cores[c].lr);
        report.push_back(cores[c].rl);
    }

    ofstream trace_out;
    MvQueueLogger logger;
    logger.bottleneck = cores[0].lr.queue;
    for (int c = 0; c < ncores; c++)
        logger.core_index[cores[c].lr.queue] = c;
    logger.full_trace = (cfg.trace == "full" && !cfg.trace_path.empty());
    if (logger.full_trace) {
        trace_out.open(cfg.trace_path.c_str());
        if (!trace_out) {
            cerr << "could not open trace " << cfg.trace_path << "\n";
            return 1;
        }
        trace_out << "time_us,queue_bytes\n";
        logger.trace = &trace_out;
    }
    for (size_t i = 0; i < queues.size(); i++)
        queues[i]->setLogger(&logger);

    struct Live {
        RoceSrc* src;
        RoceSink* sink;
        int id;
        int host_src;
        int host_dst;
        int core;
        uint64_t nominal;
        uint64_t wire;
        double rate_gbps;
        double scheduled_us;
        bool saw_core;
    };
    vector<Live> flows;
    for (size_t i = 0; i < cfg.flows.size(); i++) {
        FlowSpec& spec = cfg.flows[i];
        Route* fwd = new Route();
        fwd->push_back(left_up[spec.src].queue);
        fwd->push_back(left_up[spec.src].pipe);
        fwd->push_back(cores[spec.core].lr.queue);
        fwd->push_back(cores[spec.core].lr.pipe);
        fwd->push_back(right_down[spec.dst].queue);
        fwd->push_back(right_down[spec.dst].pipe);
        bool saw = false;
        for (size_t h = 0; h < fwd->size(); h++) {
            if (fwd->at(h) == cores[spec.core].lr.queue)
                saw = true;
        }
        Route* rev = new Route();
        rev->push_back(right_up[spec.dst].queue);
        rev->push_back(right_up[spec.dst].pipe);
        rev->push_back(cores[spec.core].rl.queue);
        rev->push_back(cores[spec.core].rl.pipe);
        rev->push_back(left_down[spec.src].queue);
        rev->push_back(left_down[spec.src].pipe);

        RoceSrc* src = new RoceSrc(NULL, NULL, eventlist, speedFromGbps(spec.rate_gbps));
        RoceSink* snk = new RoceSink();
        src->set_flowsize(spec.wire);
        src->set_flowid((flowid_t)i);
        src->set_dst((uint32_t)spec.dst);
        snk->set_src((uint32_t)spec.src);
        src->setName("mv_" + to_string(i));
        snk->setName("mv_sink_" + to_string(i));
        HostQueue* hostq = dynamic_cast<HostQueue*>(fwd->at(0));
        if (!hostq) {
            cerr << "sender access queue is not a HostQueue\n";
            return 1;
        }
        hostq->addHostSender(src);
        Route* data = new Route(*fwd, *snk);
        Route* ack = new Route(*rev, *src);
        check_non_null(data);
        check_non_null(ack);
        src->connect(data, ack, *snk, timeFromSec(spec.start_us * 1e-6));

        Live live;
        live.src = src;
        live.sink = snk;
        live.id = (int)i;
        live.host_src = spec.src;
        live.host_dst = spec.dst;
        live.core = spec.core;
        live.nominal = spec.nominal;
        live.wire = spec.wire;
        live.rate_gbps = spec.rate_gbps;
        live.scheduled_us = spec.start_us;
        live.saw_core = saw;
        flows.push_back(live);
    }

    while (EventList::doNextEvent()) {
    }

    uint64_t drops = 0, strips = 0, bounces = 0;
    for (size_t i = 0; i < queues.size(); i++) {
        Queue* qq = dynamic_cast<Queue*>(queues[i]);
        if (qq)
            drops += (uint64_t)qq->num_drops();
        CompositeQueue* cq = dynamic_cast<CompositeQueue*>(queues[i]);
        if (cq) {
            strips += (uint64_t)cq->num_stripped();
            bounces += (uint64_t)cq->num_bounced();
        }
    }

    simtime_picosec tmax = 0;
    simtime_picosec tmin_start = 0;
    bool any = false;
    uint64_t rtx = 0, nacks = 0, pauses = 0, gaps = 0, finished = 0;
    bool packets_ok = true;
    bool core_ok = true;
    double fct_sum = 0;
    double fct_max = 0;
    for (size_t i = 0; i < flows.size(); i++) {
        RoceSrc* src = flows[i].src;
        rtx += src->_rtx_packets_sent;
        nacks += src->_nacks_received;
        pauses += src->_pauses;
        gaps += flows[i].sink->_drops;
        if (!flows[i].saw_core)
            core_ok = false;
        bool done = src->_flow_started && src->_completion_time > 0;
        uint64_t expect_pkts = flows[i].wire / (uint64_t)cfg.mss;
        if (!done || src->_packets_sent != expect_pkts)
            packets_ok = false;
        double fct = 0;
        if (done) {
            finished++;
            if (!any || src->_start_time < tmin_start)
                tmin_start = src->_start_time;
            any = true;
            if (src->_completion_time > tmax)
                tmax = src->_completion_time;
            fct = timeAsUs(src->_completion_time - src->_start_time);
            fct_sum += fct;
            if (fct > fct_max)
                fct_max = fct;
        }
        double gbps = (done && fct > 0) ? ((double)flows[i].wire * 8.0 / (fct * 1e3)) : 0;
        cout << "FLOW"
             << " flow_id=" << flows[i].id
             << " role=" << (i == 0 ? "fg" : "bg")
             << " src=" << flows[i].host_src
             << " dst=" << flows[i].host_dst
             << " core=" << flows[i].core
             << " nominal_bytes=" << flows[i].nominal
             << " wire_bytes=" << flows[i].wire
             << " configured_rate_gbps=" << sci(flows[i].rate_gbps)
             << " scheduled_start_us=" << sci(flows[i].scheduled_us)
             << " actual_start_us=" << sci(done ? timeAsUs(src->_start_time) : 0)
             << " start_us=" << sci(done ? timeAsUs(src->_start_time) : 0)
             << " finish_us=" << sci(done ? timeAsUs(src->_completion_time) : 0)
             << " fct_us=" << sci(fct)
             << " achieved_throughput_gbps=" << sci(gbps)
             << " packets_sent=" << src->_packets_sent
             << " drops=" << flows[i].sink->_drops
             << " retransmissions=" << src->_rtx_packets_sent
             << "\n";
    }

    bool fg_done = flows[0].src->_flow_started && flows[0].src->_completion_time > 0;
    simtime_picosec fg_t0 = fg_done ? flows[0].src->_start_time : 0;
    simtime_picosec fg_t1 = fg_done ? flows[0].src->_completion_time : 0;
    uint64_t shared_bytes = 0, any_bytes = 0;
    if (fg_done) {
        for (size_t i = 0; i < logger.services.size(); i++) {
            SvcRec& rec = logger.services[i];
            if (rec.flow == 0 || rec.t < fg_t0 || rec.t > fg_t1)
                continue;
            any_bytes += rec.bytes;
            if (rec.core == flows[0].core)
                shared_bytes += rec.bytes;
        }
    }
    double bps = (double)speedFromGbps(cfg.bottleneck_gbps);
    double busy_us = (bps > 0) ? (shared_bytes * 8.0 / bps * 1e6) : 0;
    double fg_fct = fg_done ? timeAsUs(fg_t1 - fg_t0) : 0;

    logger.close_integral(cores[0].lr.queue, tmax);
    QStat core = logger.stats[cores[0].lr.queue];
    double cct_us = (any && tmax >= tmin_start) ? timeAsUs(tmax - tmin_start) : 0;
    double mean_fct = finished ? fct_sum / (double)finished : 0;
    double mean_q = (tmax > 0) ? core.integral_byte_ps / (double)tmax : 0;
    double integral_byte_us = core.integral_byte_ps / 1e6;
    double util = (cct_us > 0 && bps > 0) ? (core.bytes_served * 8.0) / (bps * cct_us * 1e-6) : 0;

    vector<string> reasons;
    if (finished != flows.size())
        reasons.push_back("incomplete");
    if (drops)
        reasons.push_back("drops");
    if (strips)
        reasons.push_back("strips");
    if (bounces)
        reasons.push_back("bounces");
    if (rtx)
        reasons.push_back("retransmissions");
    if (nacks)
        reasons.push_back("nacks");
    if (pauses)
        reasons.push_back("pfc");
    if (gaps)
        reasons.push_back("sink_gaps");
    if (!packets_ok)
        reasons.push_back("wire_bytes");
    if (!core_ok)
        reasons.push_back("bottleneck_not_traversed");
    if (!(fg_fct > 0))
        reasons.push_back("nonsensical_runtime");
    string reason = "none";
    if (!reasons.empty()) {
        reason = reasons[0];
        for (size_t i = 1; i < reasons.size(); i++)
            reason += "+" + reasons[i];
    }
    bool valid = reasons.empty();
    int senders = (int)left_up.size();
    int receivers = (int)right_up.size();

    cout << "RECORD"
         << " run_id=" << sanitize(cfg.run_id)
         << " sender_count=" << senders
         << " receiver_count=" << receivers
         << " nominal_message_bytes=" << flows[0].nominal
         << " wire_message_bytes=" << flows[0].wire
         << " packet_count=" << (flows[0].wire / (uint64_t)cfg.mss)
         << " mss_bytes=" << cfg.mss
         << " sender_rate_gbps=" << sci(flows[0].rate_gbps)
         << " sender_access_gbps=" << sci(cfg.sender_access_gbps)
         << " receiver_access_gbps=" << sci(cfg.receiver_access_gbps)
         << " bottleneck_gbps=" << sci(cfg.bottleneck_gbps)
         << " bottleneck_queue_bytes=" << cfg.queue_bytes
         << " propagation_us=" << sci(cfg.propagation_us)
         << " seed=" << cfg.seed
         << " transport=paced_roce"
         << " ecn_enabled=0"
         << " pfc_enabled=0"
         << " sim_cct_us=" << sci(cct_us)
         << " fg_fct_us=" << sci(fg_fct)
         << " fg_finish_us=" << sci(fg_done ? timeAsUs(fg_t1) : 0)
         << " bg_shared_bytes=" << shared_bytes
         << " bg_any_bytes=" << any_bytes
         << " bg_shared_busy_us=" << sci(busy_us)
         << " mean_flow_fct_us=" << sci(mean_fct)
         << " max_flow_fct_us=" << sci(fct_max)
         << " peak_queue_bytes=" << core.peak
         << " mean_queue_bytes=" << sci(mean_q)
         << " queue_integral_byte_us=" << sci(integral_byte_us)
         << " bottleneck_bytes_served=" << core.bytes_served
         << " bottleneck_utilization=" << sci(util)
         << " ecn_marks=" << core.ecn_marks
         << " pfc_events=" << pauses
         << " drops=" << drops
         << " strips=" << strips
         << " bounces=" << bounces
         << " retransmissions=" << rtx
         << " nacks=" << nacks
         << " sink_gaps=" << gaps
         << " bottleneck_traversed=" << (core_ok ? 1 : 0)
         << " packets_ok=" << (packets_ok ? 1 : 0)
         << " valid=" << (valid ? 1 : 0)
         << " invalid_reason=" << reason
         << "\n";

    for (size_t i = 0; i < report.size(); i++) {
        Hop& h = report[i];
        QStat st = logger.stats[h.queue];
        uint64_t qd = 0;
        Queue* qq = dynamic_cast<Queue*>(h.queue);
        if (qq)
            qd = (uint64_t)qq->num_drops();
        cout << "LINK"
             << " link_id=" << sanitize(h.name)
             << " capacity_gbps=" << sci(h.capacity_gbps)
             << " propagation_us=" << sci(cfg.propagation_us)
             << " bytes_served=" << st.bytes_served
             << " peak_queue_bytes=" << st.peak
             << " ecn_marks=" << st.ecn_marks
             << " drops=" << qd
             << " bottleneck=" << (h.bottleneck ? 1 : 0)
             << "\n";
    }
    return valid ? 0 : 2;
}

int main(int argc, char** argv) {
    MvConfig cfg;
    cfg.run_id = "unnamed";
    cfg.senders = 0;
    cfg.nominal_bytes = 0;
    cfg.wire_bytes = 0;
    cfg.mss = 9000;
    cfg.sender_rate_gbps = 10;
    cfg.sender_access_gbps = 10;
    cfg.receiver_access_gbps = 10;
    cfg.bottleneck_gbps = 10;
    cfg.propagation_us = 1;
    cfg.queue_bytes = 0;
    cfg.seed = 1;
    cfg.endtime_s = 10;
    cfg.trace = "summary";

    for (int i = 1; i < argc; i++) {
        string val;
        if (take(i, argc, argv, "--run-id", val)) cfg.run_id = val;
        else if (take(i, argc, argv, "--senders", val)) cfg.senders = atoi(val.c_str());
        else if (take(i, argc, argv, "--nominal-bytes", val)) cfg.nominal_bytes = strtoull(val.c_str(), NULL, 10);
        else if (take(i, argc, argv, "--wire-bytes", val)) cfg.wire_bytes = strtoull(val.c_str(), NULL, 10);
        else if (take(i, argc, argv, "--mss", val)) cfg.mss = atoi(val.c_str());
        else if (take(i, argc, argv, "--sender-rate-gbps", val)) cfg.sender_rate_gbps = strtod(val.c_str(), NULL);
        else if (take(i, argc, argv, "--sender-access-gbps", val)) cfg.sender_access_gbps = strtod(val.c_str(), NULL);
        else if (take(i, argc, argv, "--receiver-access-gbps", val)) cfg.receiver_access_gbps = strtod(val.c_str(), NULL);
        else if (take(i, argc, argv, "--bottleneck-gbps", val)) cfg.bottleneck_gbps = strtod(val.c_str(), NULL);
        else if (take(i, argc, argv, "--propagation-us", val)) cfg.propagation_us = strtod(val.c_str(), NULL);
        else if (take(i, argc, argv, "--queue-bytes", val)) cfg.queue_bytes = strtoull(val.c_str(), NULL, 10);
        else if (take(i, argc, argv, "--seed", val)) cfg.seed = (unsigned)atoi(val.c_str());
        else if (take(i, argc, argv, "--endtime-s", val)) cfg.endtime_s = strtod(val.c_str(), NULL);
        else if (take(i, argc, argv, "--trace", val)) cfg.trace = val;
        else if (take(i, argc, argv, "--trace-path", val)) cfg.trace_path = val;
        else if (take(i, argc, argv, "--starts-us", val)) {
            stringstream ss(val);
            string part;
            while (getline(ss, part, ',')) {
                if (part.empty())
                    continue;
                cfg.starts_us.push_back(strtod(part.c_str(), NULL));
            }
        }
        else if (take(i, argc, argv, "--flow", val)) {
            FlowSpec spec;
            char comma;
            stringstream ss(val);
            if (!(ss >> spec.src >> comma >> spec.dst >> comma >> spec.nominal >> comma
                  >> spec.wire >> comma >> spec.rate_gbps >> comma >> spec.start_us >> comma
                  >> spec.core) || spec.core < 0 || spec.start_us < 0 || spec.rate_gbps <= 0
                || spec.wire == 0 || spec.src == spec.dst) {
                cerr << "bad --flow " << val << "\n";
                return 1;
            }
            cfg.flows.push_back(spec);
        }
        else {
            usage();
            return 1;
        }
    }
    if (!cfg.flows.empty())
        return run_scheduled(cfg);
    if (cfg.senders < 1 || cfg.wire_bytes == 0 || cfg.mss <= 0 ||
        (cfg.wire_bytes % (uint64_t)cfg.mss) != 0 || cfg.queue_bytes == 0 ||
        cfg.sender_rate_gbps <= 0 || cfg.bottleneck_gbps <= 0) {
        usage();
        return 1;
    }
    if (cfg.starts_us.empty())
        cfg.starts_us.assign(cfg.senders, 0);
    if ((int)cfg.starts_us.size() != cfg.senders) {
        cerr << "--starts-us needs one value per sender\n";
        return 1;
    }
    for (size_t i = 0; i < cfg.starts_us.size(); i++) {
        if (cfg.starts_us[i] < 0) {
            cerr << "start times must be non-negative\n";
            return 1;
        }
    }

    EventList eventlist;
    srand(cfg.seed);
    Packet::set_packet_size(cfg.mss);
    EventList::setEndtime(timeFromSec(cfg.endtime_s));

    simtime_picosec hop = timeFromSec(cfg.propagation_us * 1e-6);
    mem_b access_depth = memFromPkt(1000);
    vector<Hop> hops;
    vector<BaseQueue*> queues;

    vector<Hop> sender_up(cfg.senders);
    vector<Hop> sender_down(cfg.senders);
    vector<Hop> receiver_up(cfg.senders);
    vector<Hop> receiver_down(cfg.senders);

    for (int i = 0; i < cfg.senders; i++) {
        FairPriorityQueue* up = new FairPriorityQueue(speedFromGbps(cfg.sender_access_gbps),
                                                       access_depth, eventlist, NULL);
        up->forceName("SRC" + to_string(i) + "->L");
        sender_up[i].name = up->str();
        sender_up[i].queue = up;
        sender_up[i].pipe = new Pipe(hop, eventlist);
        sender_up[i].pipe->setName("Pipe-" + sender_up[i].name);
        sender_up[i].capacity_gbps = cfg.sender_access_gbps;
        sender_up[i].bottleneck = false;
        queues.push_back(up);

        CompositeQueue* down = new CompositeQueue(speedFromGbps(cfg.sender_access_gbps),
                                                   access_depth, eventlist, NULL);
        down->forceName("L->SRC" + to_string(i));
        sender_down[i].name = down->str();
        sender_down[i].queue = down;
        sender_down[i].pipe = new Pipe(hop, eventlist);
        sender_down[i].pipe->setName("Pipe-" + sender_down[i].name);
        sender_down[i].capacity_gbps = cfg.sender_access_gbps;
        sender_down[i].bottleneck = false;
        queues.push_back(down);

        int dst = cfg.senders + i;
        FairPriorityQueue* rup = new FairPriorityQueue(speedFromGbps(cfg.receiver_access_gbps),
                                                        access_depth, eventlist, NULL);
        rup->forceName("SRC" + to_string(dst) + "->R");
        receiver_up[i].name = rup->str();
        receiver_up[i].queue = rup;
        receiver_up[i].pipe = new Pipe(hop, eventlist);
        receiver_up[i].pipe->setName("Pipe-" + receiver_up[i].name);
        receiver_up[i].capacity_gbps = cfg.receiver_access_gbps;
        receiver_up[i].bottleneck = false;
        queues.push_back(rup);

        CompositeQueue* rdown = new CompositeQueue(speedFromGbps(cfg.receiver_access_gbps),
                                                    access_depth, eventlist, NULL);
        rdown->forceName("R->DST" + to_string(dst));
        receiver_down[i].name = rdown->str();
        receiver_down[i].queue = rdown;
        receiver_down[i].pipe = new Pipe(hop, eventlist);
        receiver_down[i].pipe->setName("Pipe-" + receiver_down[i].name);
        receiver_down[i].capacity_gbps = cfg.receiver_access_gbps;
        receiver_down[i].bottleneck = false;
        queues.push_back(rdown);
    }

    CompositeQueue* core_lr_q = new CompositeQueue(speedFromGbps(cfg.bottleneck_gbps),
                                                    (mem_b)cfg.queue_bytes, eventlist, NULL);
    core_lr_q->forceName("L->R");
    Hop core_lr;
    core_lr.name = core_lr_q->str();
    core_lr.queue = core_lr_q;
    core_lr.pipe = new Pipe(hop, eventlist);
    core_lr.pipe->setName("Pipe-L->R");
    core_lr.capacity_gbps = cfg.bottleneck_gbps;
    core_lr.bottleneck = true;
    queues.push_back(core_lr_q);

    CompositeQueue* core_rl_q = new CompositeQueue(speedFromGbps(cfg.bottleneck_gbps),
                                                    access_depth, eventlist, NULL);
    core_rl_q->forceName("R->L");
    Hop core_rl;
    core_rl.name = core_rl_q->str();
    core_rl.queue = core_rl_q;
    core_rl.pipe = new Pipe(hop, eventlist);
    core_rl.pipe->setName("Pipe-R->L");
    core_rl.capacity_gbps = cfg.bottleneck_gbps;
    core_rl.bottleneck = false;
    queues.push_back(core_rl_q);

    ofstream trace_out;
    MvQueueLogger logger;
    logger.bottleneck = core_lr_q;
    logger.full_trace = (cfg.trace == "full" && !cfg.trace_path.empty());
    if (logger.full_trace) {
        trace_out.open(cfg.trace_path.c_str());
        if (!trace_out) {
            cerr << "could not open trace " << cfg.trace_path << "\n";
            return 1;
        }
        trace_out << "time_us,queue_bytes\n";
        logger.trace = &trace_out;
    }
    for (size_t i = 0; i < queues.size(); i++)
        queues[i]->setLogger(&logger);

    linkspeed_bps pace = speedFromGbps(cfg.sender_rate_gbps);
    struct Live {
        RoceSrc* src;
        RoceSink* sink;
        int id;
        int host_src;
        int host_dst;
        bool saw_bottleneck;
        double scheduled_us;
    };
    vector<Live> flows;
    for (int i = 0; i < cfg.senders; i++) {
        Route* fwd = new Route();
        fwd->push_back(sender_up[i].queue);
        fwd->push_back(sender_up[i].pipe);
        fwd->push_back(core_lr.queue);
        fwd->push_back(core_lr.pipe);
        fwd->push_back(receiver_down[i].queue);
        fwd->push_back(receiver_down[i].pipe);
        bool saw = false;
        for (size_t h = 0; h < fwd->size(); h++) {
            if (fwd->at(h) == core_lr.queue)
                saw = true;
        }
        Route* rev = new Route();
        rev->push_back(receiver_up[i].queue);
        rev->push_back(receiver_up[i].pipe);
        rev->push_back(core_rl.queue);
        rev->push_back(core_rl.pipe);
        rev->push_back(sender_down[i].queue);
        rev->push_back(sender_down[i].pipe);

        RoceSrc* src = new RoceSrc(NULL, NULL, eventlist, pace);
        RoceSink* snk = new RoceSink();
        src->set_flowsize(cfg.wire_bytes);
        src->set_dst((uint32_t)(cfg.senders + i));
        snk->set_src((uint32_t)i);
        src->setName("mv_" + to_string(i));
        snk->setName("mv_sink_" + to_string(i));
        HostQueue* hostq = dynamic_cast<HostQueue*>(fwd->at(0));
        if (!hostq) {
            cerr << "sender access queue is not a HostQueue\n";
            return 1;
        }
        hostq->addHostSender(src);
        Route* data = new Route(*fwd, *snk);
        Route* ack = new Route(*rev, *src);
        check_non_null(data);
        check_non_null(ack);
        simtime_picosec start_at = timeFromSec(cfg.starts_us[i] * 1e-6);
        src->connect(data, ack, *snk, start_at);

        Live live;
        live.src = src;
        live.sink = snk;
        live.id = i;
        live.host_src = i;
        live.host_dst = cfg.senders + i;
        live.saw_bottleneck = saw;
        live.scheduled_us = cfg.starts_us[i];
        flows.push_back(live);
    }

    while (EventList::doNextEvent()) {
    }

    uint64_t drops = 0, strips = 0, bounces = 0;
    for (size_t i = 0; i < queues.size(); i++) {
        Queue* qq = dynamic_cast<Queue*>(queues[i]);
        if (qq)
            drops += (uint64_t)qq->num_drops();
        CompositeQueue* cq = dynamic_cast<CompositeQueue*>(queues[i]);
        if (cq) {
            strips += (uint64_t)cq->num_stripped();
            bounces += (uint64_t)cq->num_bounced();
        }
    }

    simtime_picosec tmax = 0;
    simtime_picosec tmin_start = 0;
    bool any = false;
    uint64_t rtx = 0, nacks = 0, pauses = 0, gaps = 0, finished = 0;
    bool packets_ok = true;
    bool bottleneck_ok = true;
    double fct_sum = 0;
    double fct_max = 0;
    for (size_t i = 0; i < flows.size(); i++) {
        RoceSrc* src = flows[i].src;
        rtx += src->_rtx_packets_sent;
        nacks += src->_nacks_received;
        pauses += src->_pauses;
        gaps += flows[i].sink->_drops;
        if (!flows[i].saw_bottleneck)
            bottleneck_ok = false;
        bool done = src->_flow_started && src->_completion_time > 0;
        uint64_t expect_pkts = cfg.wire_bytes / (uint64_t)cfg.mss;
        if (!done || src->_packets_sent != expect_pkts)
            packets_ok = false;
        double fct = 0;
        if (done) {
            finished++;
            if (!any || src->_start_time < tmin_start)
                tmin_start = src->_start_time;
            any = true;
            if (src->_completion_time > tmax)
                tmax = src->_completion_time;
            fct = timeAsUs(src->_completion_time - src->_start_time);
            fct_sum += fct;
            if (fct > fct_max)
                fct_max = fct;
        }
        double gbps = (done && fct > 0) ? ((double)cfg.wire_bytes * 8.0 / (fct * 1e3)) : 0;
        cout << "FLOW"
             << " flow_id=" << flows[i].id
             << " src=" << flows[i].host_src
             << " dst=" << flows[i].host_dst
             << " nominal_bytes=" << cfg.nominal_bytes
             << " wire_bytes=" << cfg.wire_bytes
             << " configured_rate_gbps=" << sci(cfg.sender_rate_gbps)
             << " scheduled_start_us=" << sci(flows[i].scheduled_us)
             << " actual_start_us=" << sci(done ? timeAsUs(src->_start_time) : 0)
             << " start_us=" << sci(done ? timeAsUs(src->_start_time) : 0)
             << " finish_us=" << sci(done ? timeAsUs(src->_completion_time) : 0)
             << " fct_us=" << sci(fct)
             << " achieved_throughput_gbps=" << sci(gbps)
             << " packets_sent=" << src->_packets_sent
             << " drops=" << flows[i].sink->_drops
             << " retransmissions=" << src->_rtx_packets_sent
             << "\n";
    }

    logger.close_integral(core_lr_q, tmax);
    QStat core = logger.stats[core_lr_q];
    double cct_us = (any && tmax >= tmin_start) ? timeAsUs(tmax - tmin_start) : 0;
    double mean_fct = finished ? fct_sum / (double)finished : 0;
    double mean_q = (tmax > 0) ? core.integral_byte_ps / (double)tmax : 0;
    double integral_byte_us = core.integral_byte_ps / 1e6;
    double bps = (double)speedFromGbps(cfg.bottleneck_gbps);
    double util = (cct_us > 0 && bps > 0) ? (core.bytes_served * 8.0) / (bps * cct_us * 1e-6) : 0;

    vector<string> reasons;
    if (finished != flows.size())
        reasons.push_back("incomplete");
    if (drops)
        reasons.push_back("drops");
    if (strips)
        reasons.push_back("strips");
    if (bounces)
        reasons.push_back("bounces");
    if (rtx)
        reasons.push_back("retransmissions");
    if (nacks)
        reasons.push_back("nacks");
    if (pauses)
        reasons.push_back("pfc");
    if (gaps)
        reasons.push_back("sink_gaps");
    if (!packets_ok)
        reasons.push_back("wire_bytes");
    if (!bottleneck_ok)
        reasons.push_back("bottleneck_not_traversed");
    if (!(cct_us > 0))
        reasons.push_back("nonsensical_runtime");
    string reason = "none";
    if (!reasons.empty()) {
        reason = reasons[0];
        for (size_t i = 1; i < reasons.size(); i++)
            reason += "+" + reasons[i];
    }
    bool valid = reasons.empty();

    cout << "RECORD"
         << " run_id=" << sanitize(cfg.run_id)
         << " sender_count=" << cfg.senders
         << " receiver_count=" << cfg.senders
         << " nominal_message_bytes=" << cfg.nominal_bytes
         << " wire_message_bytes=" << cfg.wire_bytes
         << " packet_count=" << (cfg.wire_bytes / (uint64_t)cfg.mss)
         << " mss_bytes=" << cfg.mss
         << " sender_rate_gbps=" << sci(cfg.sender_rate_gbps)
         << " sender_access_gbps=" << sci(cfg.sender_access_gbps)
         << " receiver_access_gbps=" << sci(cfg.receiver_access_gbps)
         << " bottleneck_gbps=" << sci(cfg.bottleneck_gbps)
         << " bottleneck_queue_bytes=" << cfg.queue_bytes
         << " propagation_us=" << sci(cfg.propagation_us)
         << " seed=" << cfg.seed
         << " transport=paced_roce"
         << " ecn_enabled=0"
         << " pfc_enabled=0"
         << " sim_cct_us=" << sci(cct_us)
         << " mean_flow_fct_us=" << sci(mean_fct)
         << " max_flow_fct_us=" << sci(fct_max)
         << " peak_queue_bytes=" << core.peak
         << " mean_queue_bytes=" << sci(mean_q)
         << " queue_integral_byte_us=" << sci(integral_byte_us)
         << " bottleneck_bytes_served=" << core.bytes_served
         << " bottleneck_utilization=" << sci(util)
         << " ecn_marks=" << core.ecn_marks
         << " pfc_events=" << pauses
         << " drops=" << drops
         << " strips=" << strips
         << " bounces=" << bounces
         << " retransmissions=" << rtx
         << " nacks=" << nacks
         << " sink_gaps=" << gaps
         << " bottleneck_traversed=" << (bottleneck_ok ? 1 : 0)
         << " packets_ok=" << (packets_ok ? 1 : 0)
         << " valid=" << (valid ? 1 : 0)
         << " invalid_reason=" << reason
         << "\n";

    vector<Hop> report;
    report.push_back(core_lr);
    report.push_back(core_rl);
    for (int i = 0; i < cfg.senders; i++) {
        report.push_back(sender_up[i]);
        report.push_back(sender_down[i]);
        report.push_back(receiver_up[i]);
        report.push_back(receiver_down[i]);
    }
    for (size_t i = 0; i < report.size(); i++) {
        Hop& h = report[i];
        QStat st = logger.stats[h.queue];
        uint64_t qd = 0;
        Queue* qq = dynamic_cast<Queue*>(h.queue);
        if (qq)
            qd = (uint64_t)qq->num_drops();
        cout << "LINK"
             << " link_id=" << sanitize(h.name)
             << " capacity_gbps=" << sci(h.capacity_gbps)
             << " propagation_us=" << sci(cfg.propagation_us)
             << " bytes_served=" << st.bytes_served
             << " peak_queue_bytes=" << st.peak
             << " ecn_marks=" << st.ecn_marks
             << " drops=" << qd
             << " bottleneck=" << (h.bottleneck ? 1 : 0)
             << "\n";
    }
    return 0;
}
