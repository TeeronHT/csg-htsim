// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-        
#include "config.h"
#include <sstream>

#include <iostream>
#include <string.h>
#include <math.h>
#include "network.h"
#include "randomqueue.h"
#include "shortflows.h"
#include "pipe.h"
#include "eventlist.h"
#include "logfile.h"
#include "loggers.h"
#include "clock.h"
#include "constant_cca_erasure.h"
#include "compositequeue.h"
//#include "firstfit.h"
#include "topology.h"
#include "connection_matrix.h"
//#include "vl2_topology.h"

#include "fat_tree_topology.h"
//#include "generic_topology.h"
//#include "oversubscribed_fat_tree_topology.h"
//#include "multihomed_fat_tree_topology.h"
//#include "star_topology.h"
//#include "bcube_topology.h"
#include <list>

// Simulation params

#define PRINT_PATHS 0

#define PERIODIC 0
#include "main.h"

uint32_t RTT = 1; // this is per link delay in us; identical RTT microseconds = 0.001 ms
#define DEFAULT_NODES 128
#define DEFAULT_QUEUE_SIZE 8

//FirstFit* ff = NULL;
//uint32_t subflow_count = 1;

//#define SWITCH_BUFFER (SERVICE * RTT / 1000)
#define USE_FIRST_FIT 0
#define FIRST_FIT_INTERVAL 100

enum NetRouteStrategy {SOURCE_ROUTE= 0, ECMP = 1, ADAPTIVE_ROUTING = 2, ECMP_ADAPTIVE = 3, RR = 4, RR_ECMP = 5};

enum HostLBStrategy {NOLB = 0, SPRAY = 1, PLB = 2, SPRAY_ADAPTIVE = 3};

EventList eventlist;


void exit_error(char* progr) {
    cout << "Usage " << progr << " [UNCOUPLED(DEFAULT)|COUPLED_INC|FULLY_COUPLED|COUPLED_EPSILON] [epsilon][COUPLED_SCALABLE_TCP" << endl;
    exit(1);
}

int main(int argc, char **argv) {
    Clock c(timeFromSec(5 / 100.), eventlist);
    uint32_t cwnd = 15, no_of_nodes = DEFAULT_NODES;
    mem_b queuesize = DEFAULT_QUEUE_SIZE;
    linkspeed_bps linkspeed = speedFromMbps((double)HOST_NIC);
    stringstream filename(ios_base::out);
    stringstream flowfilename(ios_base::out);
    uint32_t packet_size = 4000;
    uint32_t no_of_subflows = 1;
    simtime_picosec tput_sample_time = timeFromUs((uint32_t)12);
    simtime_picosec endtime = timeFromMs(1.2);
    char* tm_file = NULL;
    char* topo_file = NULL;
    NetRouteStrategy route_strategy = SOURCE_ROUTE;
    HostLBStrategy host_lb = NOLB;
    int link_failures = 0;
    double failure_pct = 0.1; // failed links have 10% bandwidth
    int plb_ecn = 0;
    queue_type queue_type = ECN;
    double rate_coef = 1.0;
    bool rts = false;
    int k = 3;
    int flaky_links = 0;
    simtime_picosec latency = 0;

    int i = 1;
    filename << "None";
    flowfilename << "flowlog.csv";

    while (i<argc) {
        if (!strcmp(argv[i],"-o")){
            filename.str(std::string());
            filename << argv[i+1] << ".out";
            flowfilename.str(std::string());
            flowfilename << argv[i+1] << "-flows.csv";
            i++;
        } else if (!strcmp(argv[i],"-of")){
            flowfilename.str(std::string());
            flowfilename << argv[i+1];
            i++;
        } else if (!strcmp(argv[i],"-nodes")){
            no_of_nodes = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-tm")){
            tm_file = argv[i+1];
            cout << "traffic matrix input file: "<< tm_file << endl;
            i++;
        } else if (!strcmp(argv[i],"-topo")){
            topo_file = argv[i+1];
            cout << "topology input file: "<< topo_file << endl;
            i++;
        } else if (!strcmp(argv[i],"-cwnd")){
            cwnd = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-linkspeed")){
            // linkspeed specified is in Mbps
            linkspeed = speedFromMbps(atof(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i],"-end")){
            endtime = timeFromUs(atof(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i],"-q")){
            queuesize = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-mtu")){
            packet_size = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-subflows")){
            no_of_subflows = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-fails")){
            link_failures = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-failpct")){
            failure_pct = stod(argv[i+1]);
            i++;
        }  else if (!strcmp(argv[i],"-plbecn")){
            plb_ecn = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-trim")){ // TODO: Get rid of these parameters which would be incoherent in this setting
            queue_type = COMPOSITE_ECN;
        } else if (!strcmp(argv[i],"-rts")){
            rts = true;
        } else if (!strcmp(argv[i],"-flakylinks")){
            flaky_links = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-lat")){
            latency = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-tsample")){
            tput_sample_time = timeFromUs((uint32_t)atoi(argv[i+1]));
            i++;            
        } else if (!strcmp(argv[i],"-ratecoef")){
            rate_coef = stod(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-k")){
            k = stoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-hostlb")){
            if (!strcmp(argv[i+1], "spray")) {
                host_lb = SPRAY;
            } else if (!strcmp(argv[i+1], "plb")) {
                host_lb = PLB;
            } else if (!strcmp(argv[i+1], "sprayad")) {
                host_lb = SPRAY_ADAPTIVE;
            } else {
                exit_error(argv[0]);
            }
            i++;
        } else if (!strcmp(argv[i],"-strat")){
            if (!strcmp(argv[i+1], "ecmp")) {
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
                route_strategy = ECMP;
            }else if (!strcmp(argv[i+1], "pkt_ar")) {
                FatTreeSwitch::set_strategy(FatTreeSwitch::ADAPTIVE_ROUTING);
                route_strategy = ADAPTIVE_ROUTING;
            } else if (!strcmp(argv[i+1], "fl_ar")) {
                FatTreeSwitch::set_strategy(FatTreeSwitch::ADAPTIVE_ROUTING);
                FatTreeSwitch::set_ar_sticky(FatTreeSwitch::PER_FLOWLET);
                route_strategy = ADAPTIVE_ROUTING;
            } else if (!strcmp(argv[i+1], "ecmp_ar")) {
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP_ADAPTIVE);
                route_strategy = ECMP_ADAPTIVE;
            } else if (!strcmp(argv[i+1], "rr")) {
                FatTreeSwitch::set_strategy(FatTreeSwitch::RR);
                route_strategy = RR;
            } else if (!strcmp(argv[i+1], "rr_ecmp")) {
                FatTreeSwitch::set_strategy(FatTreeSwitch::RR_ECMP);
                route_strategy = RR_ECMP;
            } else {
                exit_error(argv[0]);
            }
            i++;
        } else {
            exit_error(argv[i]);
        }
        i++;
    }
    Packet::set_packet_size(packet_size);
    eventlist.setEndtime(endtime);

    queuesize = queuesize*Packet::data_packet_size();
    srand(time(NULL));
    srandom(time(NULL));
      
    cout << "requested nodes " << no_of_nodes << endl;
    cout << "cwnd " << cwnd << endl;
    cout << "mtu " << packet_size << endl;
    cout << "hoststrat " << host_lb << endl;
    cout << "strategy " << route_strategy << endl;
    cout << "subflows " << no_of_subflows << endl;
      
    
    // Log of per-flow stats
    cout << "Logging flows to " << flowfilename.str() << endl;  
    std::ofstream flowlog(flowfilename.str().c_str());
    if (!flowlog){
        cout << "Can't open for writing flow log file!"<<endl;
        exit(1);
    }

#if PRINT_PATHS
    filename << ".paths";
    cout << "Logging path choices to " << filename.str() << endl;
    std::ofstream paths(filename.str().c_str());
    if (!paths){
        cout << "Can't open for writing paths file!"<<endl;
        exit(1);
    }
#endif
    
    ConstantErasureCcaSrc* sender;
    ConstantErasureCcaSink* sink;

    Route* routeout, *routein;

#ifdef FAT_TREE
    FatTreeTopology* top = new FatTreeTopology(no_of_nodes, linkspeed, queuesize, 
                                               NULL, &eventlist, NULL, queue_type, CONST_SCHEDULER, link_failures, failure_pct, rts, latency, flaky_links, timeFromUs(100.0), timeFromUs(10.0));
    // if (flaky_links > 0) {
    //     top->set_flaky_links(flaky_links, timeFromUs(100.0), timeFromUs(10.0)); // todo: parameterize this
    // }
#endif

#ifdef OV_FAT_TREE
    OversubscribedFatTreeTopology* top = new OversubscribedFatTreeTopology(lg, &eventlist,ff);
#endif

#ifdef MH_FAT_TREE
    MultihomedFatTreeTopology* top = new MultihomedFatTreeTopology(lg, &eventlist,ff);
#endif

#ifdef STAR
    StarTopology* top = new StarTopology(lg, &eventlist,ff);
#endif

#ifdef BCUBE
    BCubeTopology* top = new BCubeTopology(lg,&eventlist,ff,COMPOSITE_PRIO);
    cout << "BCUBE " << K << endl;
#endif

#ifdef VL2
    VL2Topology* top = new VL2Topology(lg,&eventlist,ff);
#endif

#ifdef GENERIC_TOPO
    GenericTopology *top = new GenericTopology(lg, &eventlist);
    if (topo_file) {
        top->load(topo_file);
    }
#endif
    
    no_of_nodes = top->no_of_nodes();
    cout << "actual nodes " << no_of_nodes << endl;

    vector<const Route*>*** net_paths;
    net_paths = new vector<const Route*>**[no_of_nodes];

    int* is_dest = new int[no_of_nodes];
    
    for (uint32_t i=0; i<no_of_nodes; i++){
        is_dest[i] = 0;
        net_paths[i] = new vector<const Route*>*[no_of_nodes];
        for (uint32_t j = 0; j<no_of_nodes; j++)
            net_paths[i][j] = NULL;
    }

    // Permutation connections
    ConnectionMatrix* conns = new ConnectionMatrix(no_of_nodes);

    if (tm_file){
        cout << "Loading connection matrix from  " << tm_file << endl;

        if (!conns->load(tm_file))
            exit(-1);
    }
    else {
        cout << "Loading connection matrix from standard input" << endl;        
        conns->load(cin);
    }

    if (conns->N != no_of_nodes){
        cout << "Connection matrix number of nodes is " << conns->N << " while I am using " << no_of_nodes << endl;
        exit(-1);
    }
    
    vector<connection*>* all_conns;

    // used just to print out stats data at the end
    list <const Route*> routes;
    
    list <ConstantErasureCcaSrc*> srcs;
    list <ConstantErasureCcaSink*> sinks;
    // initialize all sources/sinks

    uint32_t connID = 0;
    all_conns = conns->getAllConnections();
    uint32_t connCount = all_conns->size();

    for (uint32_t c = 0; c < all_conns->size(); c++){
        connection* crt = all_conns->at(c);
        uint32_t src = crt->src;
        uint32_t dest = crt->dst;
        
        connID++;
        if (!net_paths[src][dest]) {
            vector<const Route*>* paths = top->get_paths(src,dest);
            net_paths[src][dest] = paths;
            for (uint32_t p = 0; p < paths->size(); p++) {
                routes.push_back((*paths)[p]);
            }
        }
        if (!net_paths[dest][src]) {
            vector<const Route*>* paths = top->get_paths(dest,src);
            net_paths[dest][src] = paths;
        }

        double rate = (linkspeed / ((double)all_conns->size() / no_of_nodes)) / (packet_size * 8); // assumes all nodes have the same number of connections
        simtime_picosec interpacket_delay = timeFromSec(1. / (rate * rate_coef)); //+ rand() % (2*(no_of_nodes-1)); // just to keep them not perfectly in sync
        sender = new ConstantErasureCcaSrc(eventlist, src, interpacket_delay, NULL);  
        // sender->set_cwnd(cwnd*Packet::data_packet_size());

        // if (queue_type == COMPOSITE_ECN) {
        //     sender->disable_fast_recovery(); // no fast recovery when we have packet trimming
        // }
                        
        if (crt->size>0){
            sender->set_flowsize(crt->size, k);
        } 
                
        if (host_lb == PLB) {
            sender->enable_plb();
            sender->set_plb_threshold_ecn(plb_ecn);
        } else if (host_lb == SPRAY) {
            sender->set_spraying();
        }  else if (host_lb == SPRAY_ADAPTIVE) {
            sender->set_spraying();
            sender->set_adaptive();
        }
        srcs.push_back(sender);
        sink = new ConstantErasureCcaSink();
        sinks.push_back(sink);

        sender->setName("constcca_" + ntoa(src) + "_" + ntoa(dest));

        sink->setName("constcca_sink_" + ntoa(src) + "_" + ntoa(dest));

          
        if (route_strategy != SOURCE_ROUTE) {
            routeout = top->get_tor_route(src);
            routein = top->get_tor_route(dest);
        } else {
            uint32_t choice = 0;
          
#ifdef MULTI_DC
            choice = rand()%net_paths[src][dest]->size();
#endif

#ifdef FAT_TREE
            choice = rand()%net_paths[src][dest]->size();
#endif
          
#ifdef OV_FAT_TREE
            choice = rand()%net_paths[src][dest]->size();
#endif
          
#ifdef MH_FAT_TREE
            int use_all = it_sub==net_paths[src][dest]->size();

            if (use_all)
                choice = inter;
            else
                choice = rand()%net_paths[src][dest]->size();
#endif
          
#ifdef VL2
            choice = rand()%net_paths[src][dest]->size();
#endif
          
#ifdef STAR
            choice = 0;
#endif
          
#ifdef BCUBE
            //choice = inter;
            
            int min = -1, max = -1,minDist = 1000,maxDist = 0;
            if (subflow_count==1){
                //find shortest and longest path 
                for (uint32_t dd=0;dd<net_paths[src][dest]->size();dd++){
                    if (net_paths[src][dest]->at(dd)->size()<minDist){
                        minDist = net_paths[src][dest]->at(dd)->size();
                        min = dd;
                    }
                    if (net_paths[src][dest]->at(dd)->size()>maxDist){
                        maxDist = net_paths[src][dest]->at(dd)->size();
                        max = dd;
                    }
                }
                choice = min;
            } 
            else
                choice = rand()%net_paths[src][dest]->size();
#endif
            if (choice>=net_paths[src][dest]->size()){
                printf("Weird path choice %d out of %lu\n",choice,net_paths[src][dest]->size());
                exit(1);
            }
          
#if PRINT_PATHS
            for (uint32_t ll=0;ll<net_paths[src][dest]->size();ll++){
                paths << "Route from "<< ntoa(src) << " to " << ntoa(dest) << "  (" << ll << ") -> " ;
                print_path(paths,net_paths[src][dest]->at(ll));
            }
#endif
          
            routeout = new Route(*(net_paths[src][dest]->at(choice)));
            //routeout->push_back(swiftSnk);
            
            routein = new Route(*top->get_paths(dest,src)->at(choice));
            //routein->push_back(swiftSrc);
        }

        // simtime_picosec offset = (interpacket_delay/connCount) * (rand()%(connCount-1));
        // simtime_picosec starttime = crt->start + offset;
        // cout << "starttime " << timeAsUs(starttime) << endl;
        sender->connect(*sink, (uint32_t)crt->start + rand()%(interpacket_delay), dest, *routeout, *routein);
        // sender->set_paths(net_paths[src][dest]);

        if (route_strategy != SOURCE_ROUTE) {
            top->add_host_port(src, sender->flow().flow_id(), sender);
            top->add_host_port(dest, sender->flow().flow_id(), sink);
        }
    }
    //    ShortFlows* sf = new ShortFlows(2560, eventlist, net_paths,conns,lg, &swiftRtxScanner);

    cout << "Loaded " << connID << " connections in total\n";

    // GO!
    cout << "Starting simulation" << endl;
    simtime_picosec checkpoint = timeFromUs(100.0);
    while (eventlist.doNextEvent()) {
        if (eventlist.now() > checkpoint) {
            cout << "Simulation time " << timeAsUs(eventlist.now()) << endl;
            checkpoint += timeFromUs(100.0);
            if (endtime == 0) {
                // Iterate through sinks to see if they have completed the flows
                bool all_done = true;
                list <ConstantErasureCcaSink*>::iterator sink_i;
                for (sink_i = sinks.begin(); sink_i != sinks.end(); sink_i++) {
                    if ((*sink_i)->_src->_completion_time == 0) {
                        all_done = false;
                        break;
                    }
                }
                if (all_done) {
                    cout << "All flows completed" << endl;
                    break;
                }
            }
        }
    }

    cout << "Done" << endl;

#if PRINT_PATHS
    list <const Route*>::iterator rt_i;
    int counts[10]; int hop;
    for (rt_i = routes.begin(); rt_i != routes.end(); rt_i++) {
        const Route* r = (*rt_i);
        //print_route(*r);
        cout << "Path:" << endl;
        for (uint32_t i = 0; i < r->size(); i++) {
            PacketSink *ps = r->at(i); 
            CompositeQueue *q = dynamic_cast<CompositeQueue*>(ps);
            if (q == 0) {
                cout << ps->nodename() << endl;
            } else {
                cout << q->nodename() << " id=" << q->get_id() << " " << q->num_packets() << "pkts " 
                     << q->num_headers() << "hdrs " << q->num_acks() << "acks " << q->num_nacks() << "nacks " << q->num_stripped() << "stripped"
                     << endl;
            }
        } 
        cout << endl;
    }
    for (uint32_t ix = 0; ix < 10; ix++)
        counts[ix] = 0;
    for (rt_i = routes.begin(); rt_i != routes.end(); rt_i++) {
        const Route* r = (*rt_i);
        //print_route(*r);
        hop = 0;
        for (uint32_t i = 0; i < r->size(); i++) {
            PacketSink *ps = r->at(i); 
            CompositeQueue *q = dynamic_cast<CompositeQueue*>(ps);
            if (q == 0) {
            } else {
                counts[hop] += q->num_stripped();
                q->_num_stripped = 0;
                hop++;
            }
        } 
        cout << endl;
    }
#endif    

    /*for (uint32_t i = 0; i < 10; i++)
        cout << "Hop " << i << " Count " << counts[i] << endl; */
    list <ConstantErasureCcaSrc*>::iterator src_i;
    // for (src_i = swift_srcs.begin(); src_i != swift_srcs.end(); src_i++) {
    //     cout << "Src, sent: " << (*src_i)->_highest_dsn_sent << "[rtx: " << (*src_i)->_subs[0]. << "] nacks: " << (*src_i)->_nacks_received << " pulls: " << (*src_i)->_pulls_received << " paths: " << (*src_i)->_paths.size() << endl;
    // }
    flowlog << "Flow ID,Src->Dest,Completion Time,ReceivedBytes,PacketsSent,InterDC" << endl;
    for (src_i = srcs.begin(); src_i != srcs.end(); src_i++) {
        ConstantErasureCcaSink* sink = (*src_i)->_sink;
        simtime_picosec time = (*src_i)->_completion_time > 0 ? (*src_i)->_completion_time - (*src_i)->_start_time: 0;
        flowlog << (*src_i)->get_id() << "," << (*src_i)->_addr << "->" << (*src_i)->_destination << "," << time << "," << sink->cumulative_ack() << "," << (*src_i)->_packets_sent << "," <<  "0" << endl;
    }
    flowlog.close();
    list <ConstantErasureCcaSink*>::iterator sink_i;
    for (sink_i = sinks.begin(); sink_i != sinks.end(); sink_i++) {
        cout << (*sink_i)->nodename() << " received " << (*sink_i)->cumulative_ack() << " bytes" << endl;
        if ((*sink_i)->cumulative_ack() < 2004000) { // todo: change thsi
            cout << "Incomplete flow " << endl;
            ConstantErasureCcaSink* sink = (*sink_i);
            ConstantErasureCcaSrc* counterpart_src = sink->_src;
            cout << "Src, sent: " << counterpart_src->_packets_sent << "; ACKED " << counterpart_src->packets_acked() << endl;
        }
    }
    /*
    uint64_t total_rtt = 0;
    cout << "RTT Histogram";
    for (uint32_t i = 0; i < 100000; i++) {
        if (SwiftSrc::_rtt_hist[i]!= 0) {
            cout << i << " " << SwiftSrc::_rtt_hist[i] << endl;
            total_rtt += SwiftSrc::_rtt_hist[i];
        }
    }
    cout << "RTT CDF";
    uint64_t cum_rtt = 0;
    for (uint32_t i = 0; i < 100000; i++) {
        if (SwiftSrc::_rtt_hist[i]!= 0) {
            cum_rtt += SwiftSrc::_rtt_hist[i];
            cout << i << " " << double(cum_rtt)/double(total_rtt) << endl;
        }
    }
    */
}
