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
#include "topology.h"
#include "connection_matrix.h"
#include "multi_datacenter/multi_datacenter_topology.h"
#include "multi_fat_tree_switch.h"
#include <list>

// Simulation params
#define PRINT_PATHS 0
#define PERIODIC 0
#include "main.h"

uint32_t RTT = 1; // this is per link delay in us; identical RTT microseconds = 0.001 ms
#define DEFAULT_NODES 128
#define DEFAULT_QUEUE_SIZE 8

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
    
    // Multi-DC specific parameters
    uint32_t num_datacenters = 2;
    uint32_t nodes_per_dc = no_of_nodes / 2;

    // speedFromGbps(100 * nodes_per_dc) consumes a massive amount of memory (25.6 Tbps, num packets in flight)
    linkspeed_bps wan_speed = speedFromGbps(100 * nodes_per_dc); // 100 Gbps WAN links
    mem_b wan_queue_size; // Will be initialized after packet size is set
    simtime_picosec wan_delay = timeFromUs((uint32_t)1); // 1us WAN latency

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
            nodes_per_dc = no_of_nodes / num_datacenters;
            i++;
        } else if (!strcmp(argv[i],"-dcs")){
            num_datacenters = atoi(argv[i+1]);
            nodes_per_dc = no_of_nodes / num_datacenters;
            i++;
        } else if (!strcmp(argv[i],"-wan_speed")){
            wan_speed = speedFromGbps(atof(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i],"-wan_delay")){
            wan_delay = timeFromMs(atof(argv[i+1]));
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
                MultiFatTreeSwitch::set_strategy(MultiFatTreeSwitch::ECMP);
                route_strategy = ECMP;
            }else if (!strcmp(argv[i+1], "pkt_ar")) {
                MultiFatTreeSwitch::set_strategy(MultiFatTreeSwitch::ADAPTIVE_ROUTING);
                route_strategy = ADAPTIVE_ROUTING;
            } else if (!strcmp(argv[i+1], "fl_ar")) {
                MultiFatTreeSwitch::set_strategy(MultiFatTreeSwitch::ADAPTIVE_ROUTING);
                MultiFatTreeSwitch::set_ar_sticky(MultiFatTreeSwitch::PER_FLOWLET);
                route_strategy = ADAPTIVE_ROUTING;
            } else if (!strcmp(argv[i+1], "ecmp_ar")) {
                MultiFatTreeSwitch::set_strategy(MultiFatTreeSwitch::ECMP_ADAPTIVE);
                route_strategy = ECMP_ADAPTIVE;
            } else if (!strcmp(argv[i+1], "rr")) {
                MultiFatTreeSwitch::set_strategy(MultiFatTreeSwitch::RR);
                route_strategy = RR;
            } else if (!strcmp(argv[i+1], "rr_ecmp")) {
                MultiFatTreeSwitch::set_strategy(MultiFatTreeSwitch::RR_ECMP);
                route_strategy = RR_ECMP;
            } else {
                exit_error(argv[i]);
            }
            i++;
        } else {
            exit_error(argv[i]);
        }
        i++;
    }
    
    Packet::set_packet_size(packet_size);
    eventlist.setEndtime(endtime);

    // Initialize WAN queue size after packet size is set
    wan_queue_size = memFromPkt(5000);
    
    queuesize = queuesize*Packet::data_packet_size();
    srand(time(NULL));
    srandom(time(NULL));
      
    cout << "Multi-DC Configuration:" << endl;
    cout << "  Number of datacenters: " << num_datacenters << endl;
    cout << "  Nodes per datacenter: " << nodes_per_dc << endl;
    cout << "  Total nodes: " << (num_datacenters * nodes_per_dc) << endl;
    cout << "  WAN speed: " << speedAsGbps(wan_speed) << " Gbps" << endl;
    cout << "  WAN delay: " << timeAsMs(wan_delay) << " ms" << endl;
    cout << "  Intra-DC speed: " << speedAsGbps(linkspeed) << " Gbps" << endl;
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

    // Create multi-datacenter topology
    MultiDatacenterTopology* top = new MultiDatacenterTopology(
        num_datacenters, nodes_per_dc, linkspeed, wan_speed,
        queuesize, wan_queue_size, wan_delay, NULL, &eventlist, queue_type);
   
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
            vector<const Route*>* paths = top->get_bidir_paths(src,dest,false);
            net_paths[src][dest] = paths;
            for (uint32_t p = 0; p < paths->size(); p++) {
                routes.push_back((*paths)[p]);
            }
        }
        if (!net_paths[dest][src]) {
            vector<const Route*>* paths = top->get_bidir_paths(dest,src,false);
            net_paths[dest][src] = paths;
        }

        // Adjust rate based on whether it's inter-DC or intra-DC
        double base_rate = (linkspeed / ((double)all_conns->size() / no_of_nodes)) / (packet_size * 8);
        if (top->is_inter_dc_flow(src, dest)) {
            // Inter-DC flows use WAN bandwidth
            base_rate = (wan_speed / ((double)all_conns->size() / no_of_nodes)) / (packet_size * 8);
        }
        
        simtime_picosec interpacket_delay = timeFromSec(1. / (base_rate * rate_coef));
        sender = new ConstantErasureCcaSrc(eventlist, src, interpacket_delay, NULL);  
                        
        if (crt->size>0){
            sender->set_flowsize(crt->size, k);
            cout << "Size of packet: " << crt->size << endl;
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

        // TODO:
        // Need to to connect each sink to ToR
          
        if (route_strategy != SOURCE_ROUTE) {
            // For non-source routing, we still need to create routes for the connect call
            // Get the paths and use them to create routes
            if (!net_paths[src][dest] || net_paths[src][dest]->empty()) {
                cout << "No valid path found from " << src << " to " << dest << endl;
                continue; // Skip this connection
            }
            
            uint32_t choice = 0;
            choice = rand()%net_paths[src][dest]->size();
            
            if (choice>=net_paths[src][dest]->size()){
                printf("Weird path choice %d out of %lu\n",choice,net_paths[src][dest]->size());
                exit(1);
            }
            
            routeout = new Route(*(net_paths[src][dest]->at(choice)));
            
            // Also check the reverse path
            vector<const Route*>* reverse_paths = top->get_bidir_paths(dest,src,false);
            if (!reverse_paths || reverse_paths->empty()) {
                cout << "No valid reverse path found from " << dest << " to " << src << endl;
                continue; // Skip this connection
            }
            routein = new Route(*(reverse_paths->at(choice)));

            
        } else {
            // Check if we have valid paths
            if (!net_paths[src][dest] || net_paths[src][dest]->empty()) {
                cout << "No valid path found from " << src << " to " << dest << endl;
                continue; // Skip this connection
            }
            
            uint32_t choice = 0;
            choice = rand()%net_paths[src][dest]->size();
            
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
          
            // cout << "Creating forward route from " << src << " to " << dest << endl;
            // cout << "Route has " << net_paths[src][dest]->at(choice)->size() << " elements" << endl;
            routeout = new Route(*(net_paths[src][dest]->at(choice)));
            // cout << "Forward route created with " << routeout->size() << " hops" << endl;
            // if (routeout->size() > 0) {
            //     cout << "First element type: " << typeid(*routeout->at(0)).name() << endl;
            // }
            
            // Check the reverse path
            vector<const Route*>* reverse_paths = top->get_bidir_paths(dest,src,false);
            if (!reverse_paths || reverse_paths->empty()) {
                cout << "No valid reverse path found from " << dest << " to " << src << endl;
                continue; // Skip this connection
            }
            // cout << "Creating reverse route from " << dest << " to " << src << endl;
            routein = new Route(*(reverse_paths->at(choice)));

        }

        // cout << "About to connect sender " << src << " to sink " << dest << endl;
        try {
            sender->connect(*sink, (uint32_t)crt->start + rand()%(interpacket_delay), dest, *routeout, *routein);
            // cout << "Successfully connected sender " << src << " to sink " << dest << endl;
        } catch (const std::exception& e) {
            cout << "Exception during connection setup: " << e.what() << endl;
            cout << "Failed to connect sender " << src << " to sink " << dest << endl;
            throw;
        } catch (...) {
            cout << "Unknown exception during connection setup" << endl;
            cout << "Failed to connect sender " << src << " to sink " << dest << endl;
            throw;
        }

        if (route_strategy != SOURCE_ROUTE) {
            top->add_host_port(src, sender->flow().flow_id(), sender);
            top->add_host_port(dest, sender->flow().flow_id(), sink);
        }
    }
    
    cout << "Loaded " << connID << " connections in total\n";
    cout << "***** All connections complete, entering cleanup/statistics *****" << endl;
    cout << "Inter-DC flows: " << endl;
    for (uint32_t c = 0; c < all_conns->size(); c++){
        connection* crt = all_conns->at(c);
        if (top->is_inter_dc_flow(crt->src, crt->dst)) {
            cout << "  " << crt->src << " -> " << crt->dst << " (DC" << top->get_dc_id(crt->src) 
                 << " -> DC" << top->get_dc_id(crt->dst) << ")" << endl;
        }
    }

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

    flowlog << "Flow ID,Src->Dest,Completion Time,ReceivedBytes,PacketsSent,InterDC" << endl;
    list <ConstantErasureCcaSrc*>::iterator src_i;
    for (src_i = srcs.begin(); src_i != srcs.end(); src_i++) {
        ConstantErasureCcaSink* sink = (*src_i)->_sink;
        simtime_picosec time = (*src_i)->_completion_time > 0 ? (*src_i)->_completion_time - (*src_i)->_start_time: 0;
        bool is_inter_dc = top->is_inter_dc_flow((*src_i)->_addr, (*src_i)->_destination);
        flowlog << (*src_i)->get_id() << "," << (*src_i)->_addr << "->" << (*src_i)->_destination << "," << time << "," << sink->cumulative_ack() << "," << (*src_i)->_packets_sent << "," << (is_inter_dc ? "1" : "0") << endl;
    }
    flowlog.close();
    
    list <ConstantErasureCcaSink*>::iterator sink_i;
    for (sink_i = sinks.begin(); sink_i != sinks.end(); sink_i++) {
        ConstantErasureCcaSink* sink = (*sink_i);
        ConstantErasureCcaSrc* counterpart_src = sink->_src;
        uint32_t expected_size = counterpart_src->flow_size();
        cout << (*sink_i)->nodename() << " received " << (*sink_i)->cumulative_ack() << " bytes. Expected " << expected_size << endl;
        if ((*sink_i)->cumulative_ack() < expected_size) {
            cout << "Incomplete flow " << endl;
            cout << "Src, sent: " << counterpart_src->_packets_sent << "; ACKED " << counterpart_src->packets_acked() << endl;
            cout << "Expected: " << expected_size << " bytes, received: " << (*sink_i)->cumulative_ack() << " bytes" << endl;
        }
    }
    
    delete top;
    cout << "Deleted top" << endl;
    return 0;
} 