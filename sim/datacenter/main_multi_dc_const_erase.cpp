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
#include "queue.h"
#include "connection_matrix.h"
#include "multi_datacenter/multi_datacenter_topology.h"
#include "multi_fat_tree_switch.h"
#include <list>
#include <map>
#include <vector>
#include <algorithm>
#include <cctype>

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

// Global pointer to topology for datacenter identification
MultiDatacenterTopology* g_topology = nullptr;

// Global maps to track packet drops per flow and by switch tier
std::map<uint32_t, uint32_t> flow_drop_counts;  // flow_id -> drop count
std::map<uint32_t, std::pair<uint32_t, uint32_t> > flow_id_to_src_dest;  // flow_id -> (src, dest)

// Drop counts by switch tier: 0=ToR, 1=Aggregation, 2=Core, 3=WAN, 4=Unknown
std::map<uint32_t, uint32_t> drops_by_tier;  // tier -> drop count
std::map<uint32_t, std::map<uint32_t, uint32_t> > drops_by_tier_and_flow_type;  // tier -> (flow_type: 0=intra, 1=inter) -> drop count

// Per-switch drop tracking: switch_type_tier -> switch_id -> drop_count
std::map<int, std::map<uint32_t, uint32_t> > drops_by_switch;  // tier -> switch_id -> count
std::map<int, std::map<uint32_t, std::map<uint32_t, uint32_t> > > drops_by_switch_and_flow_type;  // tier -> switch_id -> flow_type -> count

// Inter-DC flow drops by datacenter: flow_id -> (source_dc_drops, dest_dc_drops)
std::map<uint32_t, std::pair<uint32_t, uint32_t> > inter_dc_drops_by_dc;  // flow_id -> (src_dc_drops, dest_dc_drops)

// Destination aggregation switch drop tracking (for inter-DC flows at destination DC)
// Tracks which destination aggregation switches receive drops from CS->US queues
std::map<uint32_t, uint32_t> dest_agg_switch_drops;  // agg_switch_id -> drop count

// Queue utilization tracking: queue_name -> (total_drops, sum_queue_size_at_drop, sum_max_size, count)
struct QueueUtilStats {
    uint32_t drops;
    uint64_t total_queue_size_at_drop;  // bytes
    uint64_t total_max_size;  // bytes
    uint32_t count;
};
std::map<string, QueueUtilStats> queue_utilization_stats;

// Link traffic tracking: track arrivals on specific link types
struct LinkTrafficStats {
    uint64_t packet_count;
    uint64_t byte_count;
    LinkTrafficStats() : packet_count(0), byte_count(0) {}
};
std::map<string, LinkTrafficStats> wan_to_core_traffic;  // WAN -> Core link traffic
std::map<string, LinkTrafficStats> wan_to_wan_traffic;   // WAN -> WAN link traffic
std::map<string, LinkTrafficStats> core_to_wan_traffic;  // Core -> WAN link traffic (for outbound)

// Helper function to identify switch tier from queue name
// Queue naming patterns: "LS"=ToR, "US"=Aggregation, "CS"=Core, "WAN"=WAN
int get_switch_tier_from_queue(const string& queue_name) {
    if (queue_name.find("LS") == 0 || queue_name.find("SRC") == 0) {
        return 0; // ToR (Lower Switch)
    } else if (queue_name.find("US") == 0) {
        return 1; // Aggregation (Upper Switch)
    } else if (queue_name.find("CS") == 0 || queue_name.find("Switch_Core") == 0) {
        return 2; // Core Switch
    } else if (queue_name.find("WAN") != string::npos) {
        return 3; // WAN
    }
    return 4; // Unknown
}

// Extract switch ID from queue name
// Examples: "US0->LS_1" -> 0 (agg switch), "LS5->DST10" -> 5 (ToR switch), "CS2->US3" -> 2 (core switch)
uint32_t extract_switch_id_from_queue(const string& queue_name, int tier) {
    size_t pos = string::npos;
    if (tier == 0) { // ToR
        if (queue_name.find("LS") == 0) {
            pos = 2; // Skip "LS"
        } else if (queue_name.find("SRC") == 0) {
            // SRC queues point to ToR, need to find the ToR ID after "->LS"
            size_t ls_pos = queue_name.find("->LS");
            if (ls_pos != string::npos) {
                pos = ls_pos + 4;
            }
        }
    } else if (tier == 1) { // Aggregation
        if (queue_name.find("US") == 0) {
            pos = 2; // Skip "US"
        }
    } else if (tier == 2) { // Core
        if (queue_name.find("CS") == 0) {
            pos = 2; // Skip "CS"
        } else if (queue_name.find("Switch_Core_") != string::npos) {
            pos = queue_name.find("Switch_Core_") + 13;
        }
    }
    
    if (pos != string::npos) {
        // Extract number until next non-digit character
        uint32_t id = 0;
        while (pos < queue_name.length() && isdigit(queue_name[pos])) {
            id = id * 10 + (queue_name[pos] - '0');
            pos++;
        }
        return id;
    }
    return 999999; // Unknown switch ID
}

// Determine which datacenter a switch belongs to based on switch ID and topology
// Each DC has its own topology with switches numbered 0-N independently
// We need to infer DC from queue names or use topology information
int get_datacenter_from_switch(uint32_t switch_id, int tier, uint32_t flow_id, const string& queue_name) {
    if (!g_topology || flow_id_to_src_dest.find(flow_id) == flow_id_to_src_dest.end()) {
        return -1; // Unknown
    }
    
    uint32_t src = flow_id_to_src_dest[flow_id].first;
    uint32_t dest = flow_id_to_src_dest[flow_id].second;
    uint32_t src_dc = g_topology->get_dc_id(src);
    uint32_t dest_dc = g_topology->get_dc_id(dest);
    
    // If it's a WAN drop, it's between datacenters
    if (tier == 3) {
        return -2; // WAN (between DCs)
    }
    
    // For intra-DC flows, we know it's in the source DC
    if (src_dc == dest_dc) {
        return src_dc;
    }
    
    // For inter-DC flows, try to determine DC from queue name
    // Queue names like "LS18->US16" or "US20->CS0" or "SRC128->LS32"
    // If we can extract host IDs, we can determine DC
    
    // Try to extract host IDs from queue name
    // Pattern: "SRC<host_id>->LS" or "LS<switch_id>->DST<host_id>"
    size_t src_pos = queue_name.find("SRC");
    if (src_pos != string::npos) {
        // Extract host ID after SRC
        size_t num_start = src_pos + 3;
        uint32_t host_id = 0;
        while (num_start < queue_name.length() && isdigit(queue_name[num_start])) {
            host_id = host_id * 10 + (queue_name[num_start] - '0');
            num_start++;
        }
        if (host_id > 0) {
            return g_topology->get_dc_id(host_id);
        }
    }
    
    // Pattern: "->DST<host_id>" 
    size_t dst_pos = queue_name.find("->DST");
    if (dst_pos != string::npos) {
        size_t num_start = dst_pos + 5;
        uint32_t host_id = 0;
        while (num_start < queue_name.length() && isdigit(queue_name[num_start])) {
            host_id = host_id * 10 + (queue_name[num_start] - '0');
            num_start++;
        }
        if (host_id > 0) {
            return g_topology->get_dc_id(host_id);
        }
    }
    
    // For aggregation->core queues (US->CS), we can't directly determine DC from switch IDs
    // But we can use heuristics: if it's an inter-DC flow and we're going up (US->CS),
    // it's likely in the source DC (outbound path)
    // If it's going down (CS->US), it could be either DC
    
    // Heuristic: US->CS queues for inter-DC flows are likely in source DC (outbound)
    // CS->US queues could be in either DC, but if CS ID is low, likely source DC
    if (tier == 1 && queue_name.find("US") == 0 && queue_name.find("->CS") != string::npos) {
        // Aggregation to Core - likely source DC for inter-DC flows (outbound path)
        return src_dc;
    }
    
    if (tier == 2 && queue_name.find("CS") == 0 && queue_name.find("->US") != string::npos) {
        // Core to Aggregation - could be either DC, but for inter-DC flows going to dest,
        // if we're past the WAN, it's dest DC. Without route info, we'll use a heuristic.
        // For now, assume it's destination DC if CS ID suggests it
        return dest_dc;
    }
    
    // Default: unknown, will be handled by caller
    return -1;
}

// Custom TrafficLogger to track packet drops and link arrivals
class DropTrackingLogger : public TrafficLogger {
public:
    void logTraffic(Packet& pkt, Logged& location, TrafficLogger::TrafficEvent ev) {
        string queue_name = location.str();
        
        // Track packet arrivals on WAN links
        if (ev == TrafficLogger::PKT_ARRIVE) {
            // WAN -> Core links: queues named "WAN_to_CORE<core_id>_DC<dc_id>"
            if (queue_name.find("WAN_to_CORE") == 0) {
                wan_to_core_traffic[queue_name].packet_count++;
                wan_to_core_traffic[queue_name].byte_count += pkt.size();
            }
            // Core -> WAN links: queues named "CORE<core_id>_to_WAN_DC<dc_id>"
            else if (queue_name.find("CORE") == 0 && queue_name.find("_to_WAN_DC") != string::npos) {
                core_to_wan_traffic[queue_name].packet_count++;
                core_to_wan_traffic[queue_name].byte_count += pkt.size();
            }
            // WAN -> WAN links: queues named "WAN_Queue_DC<src_dc>_to_DC<dest_dc>"
            else if (queue_name.find("WAN_Queue_DC") == 0) {
                wan_to_wan_traffic[queue_name].packet_count++;
                wan_to_wan_traffic[queue_name].byte_count += pkt.size();
            }
        }
        
        if (ev == TrafficLogger::PKT_DROP) {
            uint32_t flow_id = pkt.flow().flow_id();
            flow_drop_counts[flow_id]++;
            
            // Identify switch tier from queue name
            int tier = get_switch_tier_from_queue(queue_name);
            drops_by_tier[tier]++;
            
            // Extract switch ID
            uint32_t switch_id = extract_switch_id_from_queue(queue_name, tier);
            drops_by_switch[tier][switch_id]++;
            
            // Determine flow type (intra vs inter-DC)
            uint32_t flow_type = 0; // 0 = intra-DC, 1 = inter-DC
            if (flow_id_to_src_dest.find(flow_id) != flow_id_to_src_dest.end()) {
                uint32_t src = flow_id_to_src_dest[flow_id].first;
                uint32_t dest = flow_id_to_src_dest[flow_id].second;
                if (g_topology && g_topology->is_inter_dc_flow(src, dest)) {
                    flow_type = 1; // inter-DC
                    
                    // Track which datacenter for inter-DC flows
                    uint32_t src_dc = g_topology->get_dc_id(src);
                    uint32_t dest_dc = g_topology->get_dc_id(dest);
                    
                    // Determine which DC this drop occurred in
                    int dc_id = get_datacenter_from_switch(switch_id, tier, flow_id, queue_name);
                    if (dc_id == src_dc) {
                        inter_dc_drops_by_dc[flow_id].first++;
                    } else if (dc_id == dest_dc) {
                        inter_dc_drops_by_dc[flow_id].second++;
                        
                        // Track destination aggregation switch usage
                        // For CS->US queues in destination DC, extract the aggregation switch ID
                        if (tier == 2 && queue_name.find("CS") == 0 && queue_name.find("->US") != string::npos) {
                            // Extract aggregation switch ID from "CS2->US10(0)" format
                            size_t us_pos = queue_name.find("->US");
                            if (us_pos != string::npos) {
                                size_t num_start = us_pos + 4; // Skip "->US"
                                uint32_t agg_id = 0;
                                while (num_start < queue_name.length() && isdigit(queue_name[num_start])) {
                                    agg_id = agg_id * 10 + (queue_name[num_start] - '0');
                                    num_start++;
                                }
                                dest_agg_switch_drops[agg_id]++;
                            }
                        }
                    } else if (dc_id == -2) {
                        // WAN drop (between DCs) - don't count in either DC
                        // Could track separately if needed
                    } else {
                        // Unknown - use heuristic based on tier and direction
                        // For US->CS (aggregation to core), likely source DC (outbound)
                        // For CS->US (core to aggregation), could be dest DC (inbound)
                        if (tier == 1 && queue_name.find("US") == 0 && queue_name.find("->CS") != string::npos) {
                            inter_dc_drops_by_dc[flow_id].first++; // Source DC (outbound)
                        } else if (tier == 2 && queue_name.find("CS") == 0 && queue_name.find("->US") != string::npos) {
                            inter_dc_drops_by_dc[flow_id].second++; // Destination DC (inbound)
                            
                            // Track destination aggregation switch usage (heuristic case)
                            size_t us_pos = queue_name.find("->US");
                            if (us_pos != string::npos) {
                                size_t num_start = us_pos + 4;
                                uint32_t agg_id = 0;
                                while (num_start < queue_name.length() && isdigit(queue_name[num_start])) {
                                    agg_id = agg_id * 10 + (queue_name[num_start] - '0');
                                    num_start++;
                                }
                                dest_agg_switch_drops[agg_id]++;
                            }
                        } else {
                            // Default: attribute to source DC
                            inter_dc_drops_by_dc[flow_id].first++;
                        }
                    }
                }
            }
            
            // Track by tier and flow type
            drops_by_tier_and_flow_type[tier][flow_type]++;
            drops_by_switch_and_flow_type[tier][switch_id][flow_type]++;
            
            // Track queue utilization if location is a BaseQueue
            BaseQueue* queue = dynamic_cast<BaseQueue*>(&location);
            if (queue) {
                QueueUtilStats& stats = queue_utilization_stats[queue_name];
                stats.drops++;
                stats.total_queue_size_at_drop += queue->queuesize();
                stats.total_max_size += queue->maxsize();
                stats.count++;
            }
        }
    }
};
DropTrackingLogger* drop_logger = new DropTrackingLogger();

void exit_error(char* progr) {
    cout << "Usage " << progr << " [UNCOUPLED(DEFAULT)|COUPLED_INC|FULLY_COUPLED|COUPLED_EPSILON] [epsilon][COUPLED_SCALABLE_TCP" << endl;
    exit(1);
}

int main(int argc, char **argv) {
    Clock c(timeFromSec(5 / 100.), eventlist);
    uint32_t cwnd = 15, no_of_nodes = DEFAULT_NODES * 2; // COME UP WITH A PERMANENT FIX HERE (MOVE DOWN BELOW NUM_DATACENTERS)
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
    cout << "  Testing: Nodes per DC " << nodes_per_dc << endl;
    cout << "  Testing: WAN speed " << wan_speed << endl;
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
    wan_queue_size = memFromPkt(12000);
    
    queuesize = queuesize*Packet::data_packet_size();
    
    // Enlarge intra-DC queues to reduce packet drops and isolate load balancing effects
    // Packets are dropped in Queue::receivePacket() when _queuesize + pkt.size() > _maxsize
    // This happens in all queue types: Queue, ECNQueue, ECNPrioQueue, CompositeQueue, etc.
    // Located in sim/queue.cpp, sim/ecnqueue.cpp, sim/ecnprioqueue.cpp, etc.
    queuesize = memFromPkt(12000);
    
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
    
    // Set global topology pointer for drop tracking
    g_topology = top;
   
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

    // Count inter-DC and intra-DC flows
    uint32_t num_inter_dc_flows = 0;
    uint32_t num_intra_dc_flows = 0;
    for (uint32_t c = 0; c < all_conns->size(); c++) {
        connection* crt = all_conns->at(c);
        if (top->is_inter_dc_flow(crt->src, crt->dst)) {
            num_inter_dc_flows++;
        } else {
            num_intra_dc_flows++;
        }
    }
    num_inter_dc_flows = num_inter_dc_flows / 2;

    // Calculate bottleneck rates
    // Doesn't account for multiple CORE switches in the WAN path
    double inter_dc_bottleneck_rate = (wan_speed / ((double)num_inter_dc_flows)) / (packet_size * 8);
    // I did wan_speed / (num_inter_dc_flows / no_of_nodes) to get the fair share rate for inter-DC flows
    // This caused a huge spike in retransmission of packets but did also reduce CCT
    cout << "Testing: wan_speed " << wan_speed << endl;
    cout << "Testing: inter DC flows " << num_inter_dc_flows << endl; // DOUBLE COUNTING WITH INTER_DC_FLOWS (Just divide by 2)
    cout << "Testing: inter DC bottleneck rate " << inter_dc_bottleneck_rate << endl;

    // Use single-DC approach for intra-DC flows to prevent congestion
    // No longer over allocating to intra-DC flows, which was causing the huge spike in retransmission of packets
    double intra_dc_flow_rate = (linkspeed / ((double)all_conns->size() / no_of_nodes)) / (packet_size * 8);
    cout << "Testing: linkspeed " << linkspeed << endl;
    cout << "Testing: all connections " << all_conns->size() << endl;
    cout << "Testing: intra DC flow rate " << intra_dc_flow_rate << endl;
    cout << "Testing: number of nodes " << no_of_nodes << endl;
    
    // Determine flow rates based on bottleneck analysis
    double inter_dc_flow_rate;
    if (inter_dc_bottleneck_rate < intra_dc_flow_rate) {
        // WAN is the bottleneck, limit inter-DC flows to prevent WAN congestion
        inter_dc_flow_rate = inter_dc_bottleneck_rate;
    } else {
        // Intra-DC is the bottleneck - use fair share for inter-DC flows
        inter_dc_flow_rate = intra_dc_flow_rate;
    }

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

        // Calculate fair share rates for intra-DC and inter-DC flows
        // double intra_dc_rate = (linkspeed / ((double)all_conns->size() / no_of_nodes)) / (packet_size * 8);
        // double inter_dc_rate = (wan_speed / ((double)all_conns->size() / no_of_nodes)) / (packet_size * 8);
        
        // Use fair share approach: min(intra_DC_rate, inter_DC_rate) for each flow
        // Use the calculated rates based on bottleneck analysis
        double base_rate;
        if (top->is_inter_dc_flow(src, dest)) {
            // For inter-DC flows, use the minimum of intra-DC and inter-DC fair share rates
            // base_rate = min(intra_dc_rate, inter_dc_rate);
            base_rate = inter_dc_flow_rate;

        } else {
            // For intra-DC flows, use intra-DC rate
            // base_rate = intra_dc_rate;
            base_rate = intra_dc_flow_rate;

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

        // Store flow_id -> (src, dest) mapping for later drop analysis
        uint32_t flow_id = sender->flow().flow_id();
        flow_id_to_src_dest[flow_id] = std::make_pair(src, dest);
        
        // Set drop tracking logger on the flow
        sender->flow().set_logger(drop_logger);
        
        if (route_strategy != SOURCE_ROUTE) {
            top->add_host_port(src, sender->flow().flow_id(), sender);
            top->add_host_port(dest, sender->flow().flow_id(), sink);
        }
    }
    
    cout << "Loaded " << connID << " connections in total\n";
    // cout << "***** Fair Share Rate Summary *****" << endl;
    // double intra_dc_rate_summary = (linkspeed / ((double)all_conns->size() / no_of_nodes)) / (packet_size * 8);
    // double inter_dc_rate_summary = (wan_speed / ((double)all_conns->size() / no_of_nodes)) / (packet_size * 8);
    // cout << "Intra-DC fair share rate: " << intra_dc_rate_summary << " packets/sec" << endl;
    // cout << "Inter-DC fair share rate: " << inter_dc_rate_summary << " packets/sec" << endl;
    // cout << "Rate limiting factor: " << (intra_dc_rate_summary < inter_dc_rate_summary ? "Intra-DC" : "Inter-DC") << endl;
    
    cout << "***** Bottleneck Rate Analysis Summary *****" << endl;
    cout << "Inter-DC flows: " << num_inter_dc_flows << endl;
    cout << "Intra-DC flows: " << num_intra_dc_flows << endl;
    cout << "Inter-DC bottleneck rate: " << inter_dc_bottleneck_rate << " packets/sec" << endl;
    cout << "Intra-DC flow rate (single-DC approach): " << intra_dc_flow_rate << " packets/sec" << endl;
    cout << "Assigned inter-DC flow rate: " << inter_dc_flow_rate << " packets/sec" << endl;
    cout << "Assigned intra-DC flow rate: " << intra_dc_flow_rate << " packets/sec" << endl;
    cout << "Bottleneck limiting factor: " << (inter_dc_bottleneck_rate < intra_dc_flow_rate ? "Inter-DC (WAN)" : "Intra-DC") << endl;
    
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

    flowlog << "Flow ID,Src->Dest,Completion Time,ReceivedBytes,PacketsSent,InterDC,Drops" << endl;
    list <ConstantErasureCcaSrc*>::iterator src_i;
    for (src_i = srcs.begin(); src_i != srcs.end(); src_i++) {
        ConstantErasureCcaSink* sink = (*src_i)->_sink;
        simtime_picosec time = (*src_i)->_completion_time > 0 ? (*src_i)->_completion_time - (*src_i)->_start_time: 0;
        bool is_inter_dc = top->is_inter_dc_flow((*src_i)->_addr, (*src_i)->_destination);
        uint32_t flow_id = (*src_i)->flow().flow_id();
        uint32_t drops = (flow_drop_counts.find(flow_id) != flow_drop_counts.end()) ? flow_drop_counts[flow_id] : 0;
        flowlog << (*src_i)->get_id() << "," << (*src_i)->_addr << "->" << (*src_i)->_destination << "," << time << "," << sink->cumulative_ack() << "," << (*src_i)->_packets_sent << "," << (is_inter_dc ? "1" : "0") << "," << drops << endl;
    }
    flowlog.close();
    
    // Print packet drop statistics by flow type
    cout << "\n***** Packet Drop Statistics by Flow Type *****" << endl;
    
    uint32_t total_drops_intra_dc = 0;
    uint32_t total_drops_inter_dc = 0;
    uint32_t flows_with_drops_intra_dc = 0;
    uint32_t flows_with_drops_inter_dc = 0;
    uint32_t max_drops_intra_dc = 0;
    uint32_t max_drops_inter_dc = 0;
    uint32_t total_flows_intra_dc = 0;
    uint32_t total_flows_inter_dc = 0;
    
    // Count all flows first (from flow_id_to_src_dest)
    for (std::map<uint32_t, std::pair<uint32_t, uint32_t> >::iterator it = flow_id_to_src_dest.begin();
         it != flow_id_to_src_dest.end(); ++it) {
        uint32_t src = it->second.first;
        uint32_t dest = it->second.second;
        bool is_inter_dc = top->is_inter_dc_flow(src, dest);
        
        if (is_inter_dc) {
            total_flows_inter_dc++;
        } else {
            total_flows_intra_dc++;
        }
    }
    
    // Aggregate drops by flow type
    for (std::map<uint32_t, uint32_t>::iterator it = flow_drop_counts.begin(); 
         it != flow_drop_counts.end(); ++it) {
        uint32_t flow_id = it->first;
        uint32_t drops = it->second;
        
        if (flow_id_to_src_dest.find(flow_id) != flow_id_to_src_dest.end()) {
            uint32_t src = flow_id_to_src_dest[flow_id].first;
            uint32_t dest = flow_id_to_src_dest[flow_id].second;
            bool is_inter_dc = top->is_inter_dc_flow(src, dest);
            
            if (is_inter_dc) {
                total_drops_inter_dc += drops;
                if (drops > 0) flows_with_drops_inter_dc++;
                if (drops > max_drops_inter_dc) max_drops_inter_dc = drops;
            } else {
                total_drops_intra_dc += drops;
                if (drops > 0) flows_with_drops_intra_dc++;
                if (drops > max_drops_intra_dc) max_drops_intra_dc = drops;
            }
        }
    }
    
    cout << "Intra-DC Flows:" << endl;
    cout << "  Total flows: " << total_flows_intra_dc << endl;
    cout << "  Total drops: " << total_drops_intra_dc << endl;
    cout << "  Flows with drops: " << flows_with_drops_intra_dc << " (" 
         << (total_flows_intra_dc > 0 ? (100.0 * flows_with_drops_intra_dc / total_flows_intra_dc) : 0.0) << "%)" << endl;
    cout << "  Average drops per flow: " 
         << (total_flows_intra_dc > 0 ? (double)total_drops_intra_dc / total_flows_intra_dc : 0.0) << endl;
    cout << "  Max drops in a single flow: " << max_drops_intra_dc << endl;
    if (flows_with_drops_intra_dc > 0) {
        cout << "  Average drops (flows with drops only): " 
             << (double)total_drops_intra_dc / flows_with_drops_intra_dc << endl;
    }
    
    cout << "\nInter-DC Flows:" << endl;
    cout << "  Total flows: " << total_flows_inter_dc << endl;
    cout << "  Total drops: " << total_drops_inter_dc << endl;
    cout << "  Flows with drops: " << flows_with_drops_inter_dc << " (" 
         << (total_flows_inter_dc > 0 ? (100.0 * flows_with_drops_inter_dc / total_flows_inter_dc) : 0.0) << "%)" << endl;
    cout << "  Average drops per flow: " 
         << (total_flows_inter_dc > 0 ? (double)total_drops_inter_dc / total_flows_inter_dc : 0.0) << endl;
    cout << "  Max drops in a single flow: " << max_drops_inter_dc << endl;
    if (flows_with_drops_inter_dc > 0) {
        cout << "  Average drops (flows with drops only): " 
             << (double)total_drops_inter_dc / flows_with_drops_inter_dc << endl;
    }
    
    cout << "\nOverall:" << endl;
    cout << "  Total drops: " << (total_drops_intra_dc + total_drops_inter_dc) << endl;
    cout << "  Total flows: " << (total_flows_intra_dc + total_flows_inter_dc) << endl;
    
    // Print drop statistics by switch tier
    cout << "\n***** Packet Drop Statistics by Switch Tier *****" << endl;
    uint32_t total_drops_all_tiers = 0;
    // First calculate total
    for (int tier = 0; tier <= 4; tier++) {
        uint32_t drops = (drops_by_tier.find(tier) != drops_by_tier.end()) ? drops_by_tier[tier] : 0;
        total_drops_all_tiers += drops;
    }
    
    // Then print with percentages
    for (int tier = 0; tier <= 4; tier++) {
        uint32_t drops = (drops_by_tier.find(tier) != drops_by_tier.end()) ? drops_by_tier[tier] : 0;
        
        string tier_name;
        switch(tier) {
            case 0: tier_name = "ToR (Top of Rack)"; break;
            case 1: tier_name = "Aggregation"; break;
            case 2: tier_name = "Core"; break;
            case 3: tier_name = "WAN"; break;
            case 4: tier_name = "Unknown"; break;
            default: tier_name = "Unknown"; break;
        }
        
        if (drops > 0 || tier < 4) { // Always show tiers 0-3, even if 0 drops
            cout << "  " << tier_name << ": " << drops << " drops";
            if (total_drops_all_tiers > 0) {
                cout << " (" << (100.0 * drops / total_drops_all_tiers) << "%)";
            }
            cout << endl;
        }
    }
    cout << "  Total drops (all tiers): " << total_drops_all_tiers << endl;
    
    // Print switch tier statistics split by flow type
    cout << "\n***** Switch Tier Statistics by Flow Type *****" << endl;
    for (int tier = 0; tier <= 3; tier++) {
        string tier_name;
        switch(tier) {
            case 0: tier_name = "ToR"; break;
            case 1: tier_name = "Aggregation"; break;
            case 2: tier_name = "Core"; break;
            case 3: tier_name = "WAN"; break;
            default: tier_name = "Unknown"; break;
        }
        
        uint32_t intra_drops = (drops_by_tier_and_flow_type.find(tier) != drops_by_tier_and_flow_type.end() &&
                                drops_by_tier_and_flow_type[tier].find(0) != drops_by_tier_and_flow_type[tier].end()) ?
                               drops_by_tier_and_flow_type[tier][0] : 0;
        uint32_t inter_drops = (drops_by_tier_and_flow_type.find(tier) != drops_by_tier_and_flow_type.end() &&
                                drops_by_tier_and_flow_type[tier].find(1) != drops_by_tier_and_flow_type[tier].end()) ?
                               drops_by_tier_and_flow_type[tier][1] : 0;
        uint32_t total_tier_drops = intra_drops + inter_drops;
        
        if (total_tier_drops > 0) {
            cout << "  " << tier_name << ":" << endl;
            cout << "    Intra-DC drops: " << intra_drops;
            if (total_tier_drops > 0) cout << " (" << (100.0 * intra_drops / total_tier_drops) << "%)";
            cout << endl;
            cout << "    Inter-DC drops: " << inter_drops;
            if (total_tier_drops > 0) cout << " (" << (100.0 * inter_drops / total_tier_drops) << "%)";
            cout << endl;
            cout << "    Total: " << total_tier_drops << endl;
        }
    }
    cout << "**********************************************\n" << endl;
    
    // Print inter-DC drops by datacenter
    if (!inter_dc_drops_by_dc.empty()) {
        cout << "\n***** Inter-DC Flow Drops by Datacenter *****" << endl;
        uint32_t total_src_dc_drops = 0;
        uint32_t total_dest_dc_drops = 0;
        uint32_t flows_with_src_drops = 0;
        uint32_t flows_with_dest_drops = 0;
        
        for (std::map<uint32_t, std::pair<uint32_t, uint32_t> >::iterator it = inter_dc_drops_by_dc.begin();
             it != inter_dc_drops_by_dc.end(); ++it) {
            total_src_dc_drops += it->second.first;
            total_dest_dc_drops += it->second.second;
            if (it->second.first > 0) flows_with_src_drops++;
            if (it->second.second > 0) flows_with_dest_drops++;
        }
        
        uint32_t total_inter_dc_drops_by_dc = total_src_dc_drops + total_dest_dc_drops;
        cout << "  Source DC drops: " << total_src_dc_drops;
        if (total_inter_dc_drops_by_dc > 0) {
            cout << " (" << (100.0 * total_src_dc_drops / total_inter_dc_drops_by_dc) << "%)";
        }
        cout << " (affects " << flows_with_src_drops << " flows)" << endl;
        
        cout << "  Destination DC drops: " << total_dest_dc_drops;
        if (total_inter_dc_drops_by_dc > 0) {
            cout << " (" << (100.0 * total_dest_dc_drops / total_inter_dc_drops_by_dc) << "%)";
        }
        cout << " (affects " << flows_with_dest_drops << " flows)" << endl;
        
        cout << "  Total inter-DC drops tracked: " << total_inter_dc_drops_by_dc << endl;
        cout << "  Note: Some drops may be unclassified (WAN or unknown location)" << endl;
        cout << "**********************************************\n" << endl;
    }
    
    // Print core switch analysis (critical finding: all drops at US->CS0 queues)
    cout << "\n***** Core Switch Analysis (from Aggregation->Core Queue Drops) *****" << endl;
    
    // Analyze which core switches are receiving traffic from aggregation switches
    // by looking at queue names like "US20->CS0"
    std::map<uint32_t, uint32_t> core_switch_drops_from_queues;  // CS ID -> drop count
    
    for (std::map<string, QueueUtilStats>::iterator it = queue_utilization_stats.begin();
         it != queue_utilization_stats.end(); ++it) {
        string queue_name = it->first;
        if (queue_name.find("US") == 0 && queue_name.find("->CS") != string::npos) {
            // Extract core switch ID from "US<agg_id>->CS<core_id>"
            size_t cs_pos = queue_name.find("->CS");
            if (cs_pos != string::npos) {
                size_t num_start = cs_pos + 4; // Skip "->CS"
                uint32_t core_id = 0;
                while (num_start < queue_name.length() && isdigit(queue_name[num_start])) {
                    core_id = core_id * 10 + (queue_name[num_start] - '0');
                    num_start++;
                }
                core_switch_drops_from_queues[core_id] += it->second.drops;
            }
        }
    }
    
    if (!core_switch_drops_from_queues.empty()) {
        std::vector<std::pair<uint32_t, uint32_t> > core_switches_sorted;
        for (std::map<uint32_t, uint32_t>::iterator it = core_switch_drops_from_queues.begin();
             it != core_switch_drops_from_queues.end(); ++it) {
            core_switches_sorted.push_back(std::make_pair(it->second, it->first));
        }
        std::sort(core_switches_sorted.rbegin(), core_switches_sorted.rend());
        
        uint32_t total_core_queue_drops = 0;
        for (size_t i = 0; i < core_switches_sorted.size(); i++) {
            total_core_queue_drops += core_switches_sorted[i].first;
        }
        
        cout << "  Core Switches Receiving Drops (from Agg->Core queues):" << endl;
        for (size_t i = 0; i < core_switches_sorted.size(); i++) {
            uint32_t core_id = core_switches_sorted[i].second;
            uint32_t drops = core_switches_sorted[i].first;
            cout << "    Core Switch " << core_id << ": " << drops << " drops";
            if (total_core_queue_drops > 0) {
                cout << " (" << (100.0 * drops / total_core_queue_drops) << "%)";
            }
            cout << endl;
        }
        
        // Show summary statistics
        cout << "\n  Summary:" << endl;
        cout << "    Total core switches with drops: " << core_switches_sorted.size() << endl;
        cout << "    Total drops across all core switches: " << total_core_queue_drops << endl;
        cout << "\n  Critical Analysis:" << endl;
        if (core_switches_sorted.size() == 1) {
            cout << "    CRITICAL: All " << total_core_queue_drops << " aggregation->core drops go to CS" 
                 << core_switches_sorted[0].second << "!" << endl;
            cout << "    This indicates severe ECMP hash collision or routing misconfiguration." << endl;
            cout << "    All aggregation switches are routing to the same core switch." << endl;
            cout << "    Expected: Traffic should be distributed across multiple core switches via ECMP" << endl;
        } else {
            double avg_drops = (double)total_core_queue_drops / core_switches_sorted.size();
            uint32_t max_drops = core_switches_sorted[0].first;
            double imbalance_ratio = (avg_drops > 0) ? (double)max_drops / avg_drops : 0.0;
            
            cout << "    Total core switches receiving drops: " << core_switches_sorted.size() << endl;
            cout << "    Average drops per core switch: " << avg_drops << endl;
            cout << "    Max drops (worst switch CS" << core_switches_sorted[0].second << "): " << max_drops << endl;
            cout << "    Imbalance ratio: " << imbalance_ratio << "x" << endl;
            if (imbalance_ratio > 2.0) {
                cout << "    WARNING: Significant core switch load imbalance!" << endl;
            }
        }
        
        // Capacity analysis
        cout << "\n  Capacity vs Demand Analysis:" << endl;
        cout << "    Total drops at aggregation->core queues: " << total_core_queue_drops << endl;
        cout << "    This represents " << (100.0 * total_core_queue_drops / total_drops_all_tiers) << "% of all drops" << endl;
        if (core_switches_sorted.size() == 1) {
            cout << "    All traffic concentrated on CS" << core_switches_sorted[0].second 
                 << " - core switch is severely oversubscribed!" << endl;
        }
    } else {
        cout << "  No aggregation->core queue drops found" << endl;
    }
    cout << "**********************************************\n" << endl;
    
    // Print per-switch drop statistics (especially for aggregation tier)
    cout << "\n***** Per-Switch Drop Statistics (Aggregation Tier) *****" << endl;
    if (drops_by_switch.find(1) != drops_by_switch.end() && !drops_by_switch[1].empty()) {
        // Sort aggregation switches by drop count
        std::vector<std::pair<uint32_t, uint32_t> > agg_switches;
        for (std::map<uint32_t, uint32_t>::iterator it = drops_by_switch[1].begin();
             it != drops_by_switch[1].end(); ++it) {
            agg_switches.push_back(std::make_pair(it->second, it->first));
        }
        std::sort(agg_switches.rbegin(), agg_switches.rend());
        
        uint32_t total_agg_drops = 0;
        for (size_t i = 0; i < agg_switches.size(); i++) {
            total_agg_drops += agg_switches[i].first;
        }
        
        cout << "  Top 20 Aggregation Switches by Drop Count:" << endl;
        int count = 0;
        for (size_t i = 0; i < agg_switches.size() && count < 20; i++) {
            uint32_t switch_id = agg_switches[i].second;
            uint32_t drops = agg_switches[i].first;
            uint32_t intra_drops = (drops_by_switch_and_flow_type.find(1) != drops_by_switch_and_flow_type.end() &&
                                    drops_by_switch_and_flow_type[1].find(switch_id) != drops_by_switch_and_flow_type[1].end() &&
                                    drops_by_switch_and_flow_type[1][switch_id].find(0) != drops_by_switch_and_flow_type[1][switch_id].end()) ?
                                   drops_by_switch_and_flow_type[1][switch_id][0] : 0;
            uint32_t inter_drops = (drops_by_switch_and_flow_type.find(1) != drops_by_switch_and_flow_type.end() &&
                                    drops_by_switch_and_flow_type[1].find(switch_id) != drops_by_switch_and_flow_type[1].end() &&
                                    drops_by_switch_and_flow_type[1][switch_id].find(1) != drops_by_switch_and_flow_type[1][switch_id].end()) ?
                                   drops_by_switch_and_flow_type[1][switch_id][1] : 0;
            
            cout << "    Agg Switch " << switch_id << ": " << drops << " drops";
            if (total_agg_drops > 0) {
                cout << " (" << (100.0 * drops / total_agg_drops) << "%)";
            }
            cout << " [Intra: " << intra_drops << ", Inter: " << inter_drops << "]" << endl;
            count++;
        }
        
        // Load balancing analysis
        cout << "\n  Load Balancing Analysis:" << endl;
        if (!agg_switches.empty()) {
            double avg_drops = (double)total_agg_drops / agg_switches.size();
            uint32_t max_drops = agg_switches[0].first;
            uint32_t min_drops = agg_switches[agg_switches.size()-1].first;
            double imbalance_ratio = (avg_drops > 0) ? (double)max_drops / avg_drops : 0.0;
            
            cout << "    Total aggregation switches: " << agg_switches.size() << endl;
            cout << "    Average drops per switch: " << avg_drops << endl;
            cout << "    Max drops (worst switch): " << max_drops << endl;
            cout << "    Min drops (best switch): " << min_drops << endl;
            cout << "    Imbalance ratio (max/avg): " << imbalance_ratio << "x" << endl;
            if (imbalance_ratio > 2.0) {
                cout << "    WARNING: Significant load imbalance detected!" << endl;
            }
        }
    } else {
        cout << "  No aggregation switch drops recorded" << endl;
    }
    cout << "**********************************************\n" << endl;
    
    // Print destination aggregation switch distribution (for inter-DC flows)
    if (!dest_agg_switch_drops.empty()) {
        cout << "\n***** Destination Aggregation Switch Distribution (Inter-DC Flows) *****" << endl;
        cout << "This shows which destination aggregation switches are receiving traffic/drops" << endl;
        cout << "from inter-DC flows. Good load balancing should distribute across all switches." << endl;
        
        // Sort by drop count
        std::vector<std::pair<uint32_t, uint32_t> > agg_switches;
        for (std::map<uint32_t, uint32_t>::iterator it = dest_agg_switch_drops.begin();
             it != dest_agg_switch_drops.end(); ++it) {
            agg_switches.push_back(std::make_pair(it->second, it->first));
        }
        std::sort(agg_switches.rbegin(), agg_switches.rend());
        
        uint32_t total_dest_agg_drops = 0;
        for (size_t i = 0; i < agg_switches.size(); i++) {
            total_dest_agg_drops += agg_switches[i].first;
        }
        
        cout << "\n  Destination Aggregation Switches with Drops:" << endl;
        cout << "  Total destination aggregation switches used: " << agg_switches.size() << endl;
        cout << "  Total drops at destination aggregation switches: " << total_dest_agg_drops << endl;
        cout << "\n  Per-Switch Breakdown:" << endl;
        
        uint32_t count = 0;
        for (size_t i = 0; i < agg_switches.size() && count < 20; i++) {
            uint32_t switch_id = agg_switches[i].second;
            uint32_t drops = agg_switches[i].first;
            
            cout << "    Dest Agg Switch " << switch_id << ": " << drops << " drops";
            if (total_dest_agg_drops > 0) {
                cout << " (" << (100.0 * drops / total_dest_agg_drops) << "%)";
            }
            cout << endl;
            count++;
        }
        if (agg_switches.size() > 20) {
            cout << "    ... and " << (agg_switches.size() - 20) << " more switches" << endl;
        }
        
        // Load balancing analysis
        cout << "\n  Load Balancing Analysis:" << endl;
        if (!agg_switches.empty()) {
            double avg_drops = (double)total_dest_agg_drops / agg_switches.size();
            uint32_t max_drops = agg_switches[0].first;
            uint32_t min_drops = agg_switches[agg_switches.size()-1].first;
            double imbalance_ratio = (avg_drops > 0) ? (double)max_drops / avg_drops : 0.0;
            
            cout << "    Total destination aggregation switches: " << agg_switches.size() << endl;
            cout << "    Average drops per switch: " << avg_drops << endl;
            cout << "    Max drops (worst switch): " << max_drops << endl;
            cout << "    Min drops (best switch): " << min_drops << endl;
            cout << "    Imbalance ratio (max/avg): " << imbalance_ratio << "x" << endl;
            
            // Expected number of aggregation switches per pod (typically K/2 for K-ary fat tree)
            // For a 4-ary fat tree, we'd expect 2 aggregation switches per pod
            // For a 6-ary fat tree, we'd expect 3 aggregation switches per pod
            // We can't determine this from the data, but we can check if we're using all switches
            cout << "\n    Interpretation:" << endl;
            if (imbalance_ratio < 1.5) {
                cout << "      ✓ Good load balancing: Traffic is well distributed across destination aggregation switches" << endl;
            } else if (imbalance_ratio < 3.0) {
                cout << "      ⚠ Moderate imbalance: Some switches are receiving more traffic than others" << endl;
            } else {
                cout << "      ✗ Poor load balancing: Traffic is heavily concentrated on a few switches" << endl;
                cout << "        This suggests the route creation fix may not be fully effective" << endl;
            }
            
            if (agg_switches.size() < 4) {
                cout << "      ⚠ WARNING: Only " << agg_switches.size() << " destination aggregation switches are being used." << endl;
                cout << "        This suggests traffic is not distributed across all available paths." << endl;
            } else {
                cout << "      ✓ Multiple destination aggregation switches are being used (" << agg_switches.size() << " switches)" << endl;
            }
        }
    } else {
        cout << "\n***** Destination Aggregation Switch Distribution *****" << endl;
        cout << "  No destination aggregation switch drops recorded" << endl;
        cout << "  (This may indicate all drops are at core switches or WAN)" << endl;
    }
    cout << "**********************************************\n" << endl;
    
    // Print queue utilization statistics - show ALL queues, grouped by core switch
    if (!queue_utilization_stats.empty()) {
        cout << "\n***** Queue Utilization at Drop Time (All Queues) *****" << endl;
        
        // Group queues by core switch (for US->CS queues)
        std::map<uint32_t, std::vector<std::pair<uint32_t, string> > > queues_by_core_switch;
        std::vector<std::pair<uint32_t, string> > other_queues; // Non-US->CS queues
        
        for (std::map<string, QueueUtilStats>::iterator it = queue_utilization_stats.begin();
             it != queue_utilization_stats.end(); ++it) {
            if (it->second.count > 0 && it->second.drops > 0) {
                string queue_name = it->first;
                if (queue_name.find("US") == 0 && queue_name.find("->CS") != string::npos) {
                    // Extract core switch ID
                    size_t cs_pos = queue_name.find("->CS");
                    if (cs_pos != string::npos) {
                        size_t num_start = cs_pos + 4;
                        uint32_t core_id = 0;
                        while (num_start < queue_name.length() && isdigit(queue_name[num_start])) {
                            core_id = core_id * 10 + (queue_name[num_start] - '0');
                            num_start++;
                        }
                        queues_by_core_switch[core_id].push_back(std::make_pair(it->second.drops, queue_name));
                    }
                } else {
                    other_queues.push_back(std::make_pair(it->second.drops, queue_name));
                }
            }
        }
        
        // Sort queues within each core switch by drop count
        for (std::map<uint32_t, std::vector<std::pair<uint32_t, string> > >::iterator it = queues_by_core_switch.begin();
             it != queues_by_core_switch.end(); ++it) {
            std::sort(it->second.rbegin(), it->second.rend());
        }
        
        // Print queues grouped by core switch
        if (!queues_by_core_switch.empty()) {
            cout << "  Aggregation->Core Queues (grouped by Core Switch):" << endl;
            for (std::map<uint32_t, std::vector<std::pair<uint32_t, string> > >::iterator cs_it = queues_by_core_switch.begin();
                 cs_it != queues_by_core_switch.end(); ++cs_it) {
                uint32_t core_id = cs_it->first;
                uint32_t total_cs_drops = 0;
                for (size_t i = 0; i < cs_it->second.size(); i++) {
                    total_cs_drops += cs_it->second[i].first;
                }
                
                cout << "\n    Core Switch " << core_id << " (Total: " << total_cs_drops << " drops):" << endl;
                for (size_t i = 0; i < cs_it->second.size(); i++) {
                    string queue_name = cs_it->second[i].second;
                    uint32_t drops = cs_it->second[i].first;
                    QueueUtilStats& stats = queue_utilization_stats[queue_name];
                    double avg_util = (double)stats.total_queue_size_at_drop / stats.total_max_size;
                    double avg_queue_size = (double)stats.total_queue_size_at_drop / stats.count;
                    double avg_max_size = (double)stats.total_max_size / stats.count;
                    
                    cout << "      " << queue_name << ": " << drops << " drops, " 
                         << (avg_util * 100) << "% util, " << (avg_queue_size / 1024.0) << " KB / " 
                         << (avg_max_size / 1024.0) << " KB" << endl;
                }
            }
        }
        
        // Print other queues (non-US->CS)
        if (!other_queues.empty()) {
            std::sort(other_queues.rbegin(), other_queues.rend());
            cout << "\n  Other Queues (Top 20):" << endl;
            int count = 0;
            for (size_t i = 0; i < other_queues.size() && count < 20; i++, count++) {
                string queue_name = other_queues[i].second;
                uint32_t drops = other_queues[i].first;
                QueueUtilStats& stats = queue_utilization_stats[queue_name];
                double avg_util = (double)stats.total_queue_size_at_drop / stats.total_max_size;
                double avg_queue_size = (double)stats.total_queue_size_at_drop / stats.count;
                double avg_max_size = (double)stats.total_max_size / stats.count;
                
                cout << "    " << queue_name << ": " << drops << " drops, " 
                     << (avg_util * 100) << "% util, " << (avg_queue_size / 1024.0) << " KB / " 
                     << (avg_max_size / 1024.0) << " KB" << endl;
            }
            if (other_queues.size() > 20) {
                cout << "    ... and " << (other_queues.size() - 20) << " more queues" << endl;
            }
        }
        
        cout << "\n  Summary:" << endl;
        cout << "    Total queues with drops: " << queue_utilization_stats.size() << endl;
        cout << "    Total core switches receiving drops: " << queues_by_core_switch.size() << endl;
        if (!queues_by_core_switch.empty()) {
            cout << "    Core switches: ";
            bool first = true;
            for (std::map<uint32_t, std::vector<std::pair<uint32_t, string> > >::iterator it = queues_by_core_switch.begin();
                 it != queues_by_core_switch.end(); ++it) {
                if (!first) cout << ", ";
                cout << "CS" << it->first;
                first = false;
            }
            cout << endl;
        }
        cout << "**********************************************\n" << endl;
    }
    
    cout << "**********************************************\n" << endl;
    
    // Print flows with highest drop counts (top 10)
    cout << "\n***** Top 10 Flows by Drop Count *****" << endl;
    // Create a vector of pairs for sorting
    std::vector<std::pair<uint32_t, uint32_t> > sorted_drops;
    for (std::map<uint32_t, uint32_t>::iterator it = flow_drop_counts.begin(); 
         it != flow_drop_counts.end(); ++it) {
        if (it->second > 0) {
            sorted_drops.push_back(std::make_pair(it->second, it->first));
        }
    }
    std::sort(sorted_drops.rbegin(), sorted_drops.rend()); // Sort descending
    
    int count = 0;
    for (std::vector<std::pair<uint32_t, uint32_t> >::iterator it = sorted_drops.begin(); 
         it != sorted_drops.end() && count < 10; ++it, count++) {
        uint32_t flow_id = it->second;
        uint32_t drops = it->first;
        if (flow_id_to_src_dest.find(flow_id) != flow_id_to_src_dest.end()) {
            uint32_t src = flow_id_to_src_dest[flow_id].first;
            uint32_t dest = flow_id_to_src_dest[flow_id].second;
            bool is_inter_dc = top->is_inter_dc_flow(src, dest);
            cout << "  Flow " << flow_id << ": " << src << "->" << dest 
                 << " (" << (is_inter_dc ? "Inter-DC" : "Intra-DC") << ") - " << drops << " drops" << endl;
        }
    }
    if (sorted_drops.empty()) {
        cout << "  No drops recorded" << endl;
    }
    cout << "**********************************************\n" << endl;
    
    // Print link arrival rates and traffic statistics
    cout << "\n***** Link Traffic Statistics (Arrival Rates) *****" << endl;
    cout << "==================================================" << endl;
    cout << "This section shows traffic load on WAN links to diagnose congestion" << endl;
    
    // Calculate simulation time in seconds
    simtime_picosec sim_duration = eventlist.now();
    double sim_duration_sec = timeAsSec(sim_duration);
    
    cout << "\nSimulation duration: " << sim_duration_sec << " seconds (" 
         << timeAsMs(sim_duration) << " ms)" << endl;
    
    // Get WAN link capacity for utilization calculation
    double wan_link_capacity_gbps = speedAsGbps(wan_speed);
    cout << "WAN link capacity: " << wan_link_capacity_gbps << " Gbps per link" << endl;
    
    // WAN -> Core link traffic
    if (!wan_to_core_traffic.empty()) {
        cout << "\n========================================" << endl;
        cout << "  WAN -> Core Links (Inbound to DC)" << endl;
        cout << "========================================" << endl;
        uint64_t total_wan_to_core_packets = 0;
        uint64_t total_wan_to_core_bytes = 0;
        
        // Sort by traffic volume
        std::vector<std::pair<uint64_t, string> > wan_core_sorted;
        for (std::map<string, LinkTrafficStats>::iterator it = wan_to_core_traffic.begin();
             it != wan_to_core_traffic.end(); ++it) {
            wan_core_sorted.push_back(std::make_pair(it->second.byte_count, it->first));
            total_wan_to_core_packets += it->second.packet_count;
            total_wan_to_core_bytes += it->second.byte_count;
        }
        std::sort(wan_core_sorted.rbegin(), wan_core_sorted.rend());
        
        cout << "  Total WAN->Core links active: " << wan_to_core_traffic.size() << endl;
        cout << "  Total packets across all links: " << total_wan_to_core_packets << endl;
        cout << "  Total bytes across all links: " << total_wan_to_core_bytes << " (" 
             << (total_wan_to_core_bytes / (1024.0 * 1024.0)) << " MB)" << endl;
        cout << "  Average arrival rate: " << (total_wan_to_core_packets / sim_duration_sec) 
             << " packets/sec" << endl;
        double total_wan_core_throughput = (total_wan_to_core_bytes * 8) / sim_duration_sec / 1e9;
        cout << "  Average throughput: " << total_wan_core_throughput << " Gbps" << endl;
        
        // Calculate per-link average utilization
        if (wan_to_core_traffic.size() > 0) {
            double avg_per_link_gbps = total_wan_core_throughput / wan_to_core_traffic.size();
            double avg_utilization = (avg_per_link_gbps / wan_link_capacity_gbps) * 100.0;
            cout << "  Average per-link throughput: " << avg_per_link_gbps << " Gbps" << endl;
            cout << "  Average per-link utilization: " << avg_utilization << "%" << endl;
            if (avg_utilization > 80.0) {
                cout << "  ⚠ WARNING: High average utilization - potential congestion!" << endl;
            }
        }
        
        cout << "\n  Per-Link Breakdown (Top 20 by traffic):" << endl;
        cout << "  -----------------------------------------------" << endl;
        int count = 0;
        for (size_t i = 0; i < wan_core_sorted.size() && count < 20; i++, count++) {
            string link_name = wan_core_sorted[i].second;
            LinkTrafficStats& stats = wan_to_core_traffic[link_name];
            double arrival_rate = stats.packet_count / sim_duration_sec;
            double throughput_gbps = (stats.byte_count * 8) / sim_duration_sec / 1e9;
            double utilization = (throughput_gbps / wan_link_capacity_gbps) * 100.0;
            
            cout << "    [" << (i+1) << "] " << link_name << endl;
            cout << "        Packets: " << stats.packet_count 
                 << " | Bytes: " << (stats.byte_count / (1024.0 * 1024.0)) << " MB" << endl;
            cout << "        Arrival rate: " << arrival_rate << " pkt/sec" << endl;
            cout << "        Throughput: " << throughput_gbps << " Gbps (" 
                 << utilization << "% utilized)";
            if (utilization > 90.0) {
                cout << " ⚠ CONGESTED!";
            } else if (utilization > 70.0) {
                cout << " ⚠ High load";
            }
            cout << endl;
        }
        if (wan_core_sorted.size() > 20) {
            cout << "    ... and " << (wan_core_sorted.size() - 20) << " more links" << endl;
        }
    } else {
        cout << "\n  WAN -> Core Links: No traffic recorded" << endl;
    }
    
    // WAN -> WAN link traffic  
    if (!wan_to_wan_traffic.empty()) {
        cout << "\n========================================" << endl;
        cout << "  WAN -> WAN Links (Inter-DC Traffic)" << endl;
        cout << "========================================" << endl;
        uint64_t total_wan_to_wan_packets = 0;
        uint64_t total_wan_to_wan_bytes = 0;
        
        // Sort by traffic volume
        std::vector<std::pair<uint64_t, string> > wan_wan_sorted;
        for (std::map<string, LinkTrafficStats>::iterator it = wan_to_wan_traffic.begin();
             it != wan_to_wan_traffic.end(); ++it) {
            wan_wan_sorted.push_back(std::make_pair(it->second.byte_count, it->first));
            total_wan_to_wan_packets += it->second.packet_count;
            total_wan_to_wan_bytes += it->second.byte_count;
        }
        std::sort(wan_wan_sorted.rbegin(), wan_wan_sorted.rend());
        
        cout << "  Total WAN->WAN links active: " << wan_to_wan_traffic.size() << endl;
        cout << "  Total packets across all links: " << total_wan_to_wan_packets << endl;
        cout << "  Total bytes across all links: " << total_wan_to_wan_bytes << " (" 
             << (total_wan_to_wan_bytes / (1024.0 * 1024.0)) << " MB)" << endl;
        cout << "  Average arrival rate: " << (total_wan_to_wan_packets / sim_duration_sec) 
             << " packets/sec" << endl;
        double total_wan_wan_throughput = (total_wan_to_wan_bytes * 8) / sim_duration_sec / 1e9;
        cout << "  Average throughput: " << total_wan_wan_throughput << " Gbps" << endl;
        
        // Calculate per-link average utilization
        if (wan_to_wan_traffic.size() > 0) {
            double avg_per_link_gbps = total_wan_wan_throughput / wan_to_wan_traffic.size();
            double avg_utilization = (avg_per_link_gbps / wan_link_capacity_gbps) * 100.0;
            cout << "  Average per-link throughput: " << avg_per_link_gbps << " Gbps" << endl;
            cout << "  Average per-link utilization: " << avg_utilization << "%" << endl;
            if (avg_utilization > 80.0) {
                cout << "  ⚠ WARNING: High average utilization - WAN backbone congested!" << endl;
            }
        }
        
        cout << "\n  Per-Link Breakdown:" << endl;
        cout << "  -----------------------------------------------" << endl;
        for (size_t i = 0; i < wan_wan_sorted.size(); i++) {
            string link_name = wan_wan_sorted[i].second;
            LinkTrafficStats& stats = wan_to_wan_traffic[link_name];
            double arrival_rate = stats.packet_count / sim_duration_sec;
            double throughput_gbps = (stats.byte_count * 8) / sim_duration_sec / 1e9;
            double utilization = (throughput_gbps / wan_link_capacity_gbps) * 100.0;
            
            cout << "    [" << (i+1) << "] " << link_name << endl;
            cout << "        Packets: " << stats.packet_count 
                 << " | Bytes: " << (stats.byte_count / (1024.0 * 1024.0)) << " MB" << endl;
            cout << "        Arrival rate: " << arrival_rate << " pkt/sec" << endl;
            cout << "        Throughput: " << throughput_gbps << " Gbps (" 
                 << utilization << "% utilized)";
            if (utilization > 90.0) {
                cout << " ⚠ CONGESTED!";
            } else if (utilization > 70.0) {
                cout << " ⚠ High load";
            }
            cout << endl;
        }
    } else {
        cout << "\n========================================" << endl;
        cout << "  WAN -> WAN Links: No traffic recorded" << endl;
        cout << "========================================" << endl;
    }
    
    // Core -> WAN link traffic (for comparison)
    if (!core_to_wan_traffic.empty()) {
        cout << "\n========================================" << endl;
        cout << "  Core -> WAN Links (Outbound from DC)" << endl;
        cout << "========================================" << endl;
        uint64_t total_core_to_wan_packets = 0;
        uint64_t total_core_to_wan_bytes = 0;
        
        // Sort by traffic volume
        std::vector<std::pair<uint64_t, string> > core_wan_sorted;
        for (std::map<string, LinkTrafficStats>::iterator it = core_to_wan_traffic.begin();
             it != core_to_wan_traffic.end(); ++it) {
            core_wan_sorted.push_back(std::make_pair(it->second.byte_count, it->first));
            total_core_to_wan_packets += it->second.packet_count;
            total_core_to_wan_bytes += it->second.byte_count;
        }
        std::sort(core_wan_sorted.rbegin(), core_wan_sorted.rend());
        
        cout << "  Total Core->WAN links active: " << core_to_wan_traffic.size() << endl;
        cout << "  Total packets across all links: " << total_core_to_wan_packets << endl;
        cout << "  Total bytes across all links: " << total_core_to_wan_bytes << " (" 
             << (total_core_to_wan_bytes / (1024.0 * 1024.0)) << " MB)" << endl;
        cout << "  Average arrival rate: " << (total_core_to_wan_packets / sim_duration_sec) 
             << " packets/sec" << endl;
        double total_core_wan_throughput = (total_core_to_wan_bytes * 8) / sim_duration_sec / 1e9;
        cout << "  Average throughput: " << total_core_wan_throughput << " Gbps" << endl;
        
        // Calculate per-link average utilization
        if (core_to_wan_traffic.size() > 0) {
            double avg_per_link_gbps = total_core_wan_throughput / core_to_wan_traffic.size();
            double avg_utilization = (avg_per_link_gbps / wan_link_capacity_gbps) * 100.0;
            cout << "  Average per-link throughput: " << avg_per_link_gbps << " Gbps" << endl;
            cout << "  Average per-link utilization: " << avg_utilization << "%" << endl;
        }
        
        cout << "\n  Per-Link Breakdown (Top 10 by traffic):" << endl;
        cout << "  -----------------------------------------------" << endl;
        int count = 0;
        for (size_t i = 0; i < core_wan_sorted.size() && count < 10; i++, count++) {
            string link_name = core_wan_sorted[i].second;
            LinkTrafficStats& stats = core_to_wan_traffic[link_name];
            double arrival_rate = stats.packet_count / sim_duration_sec;
            double throughput_gbps = (stats.byte_count * 8) / sim_duration_sec / 1e9;
            double utilization = (throughput_gbps / wan_link_capacity_gbps) * 100.0;
            
            cout << "    [" << (i+1) << "] " << link_name << endl;
            cout << "        Packets: " << stats.packet_count 
                 << " | Bytes: " << (stats.byte_count / (1024.0 * 1024.0)) << " MB" << endl;
            cout << "        Arrival rate: " << arrival_rate << " pkt/sec" << endl;
            cout << "        Throughput: " << throughput_gbps << " Gbps (" 
                 << utilization << "% utilized)" << endl;
        }
        if (core_wan_sorted.size() > 10) {
            cout << "    ... and " << (core_wan_sorted.size() - 10) << " more links" << endl;
        }
    } else {
        cout << "\n========================================" << endl;
        cout << "  Core -> WAN Links: No traffic recorded" << endl;
        cout << "========================================" << endl;
    }
    
    // Add WAN traffic summary and bottleneck analysis
    cout << "\n========================================" << endl;
    cout << "  WAN Traffic Summary & Analysis" << endl;
    cout << "========================================" << endl;
    
    if (!wan_to_core_traffic.empty() || !wan_to_wan_traffic.empty() || !core_to_wan_traffic.empty()) {
        uint64_t total_wan_traffic_bytes = 0;
        int total_wan_links = 0;
        
        // Sum up all WAN-related traffic
        for (std::map<string, LinkTrafficStats>::iterator it = wan_to_core_traffic.begin();
             it != wan_to_core_traffic.end(); ++it) {
            total_wan_traffic_bytes += it->second.byte_count;
            total_wan_links++;
        }
        for (std::map<string, LinkTrafficStats>::iterator it = wan_to_wan_traffic.begin();
             it != wan_to_wan_traffic.end(); ++it) {
            total_wan_traffic_bytes += it->second.byte_count;
            total_wan_links++;
        }
        for (std::map<string, LinkTrafficStats>::iterator it = core_to_wan_traffic.begin();
             it != core_to_wan_traffic.end(); ++it) {
            total_wan_traffic_bytes += it->second.byte_count;
            total_wan_links++;
        }
        
        double total_wan_throughput_gbps = (total_wan_traffic_bytes * 8) / sim_duration_sec / 1e9;
        double total_wan_capacity_gbps = wan_link_capacity_gbps * total_wan_links;
        double overall_wan_utilization = (total_wan_throughput_gbps / total_wan_capacity_gbps) * 100.0;
        
        cout << "  Total WAN-related links: " << total_wan_links << endl;
        cout << "  Total WAN traffic: " << (total_wan_traffic_bytes / (1024.0 * 1024.0)) << " MB" << endl;
        cout << "  Total WAN throughput: " << total_wan_throughput_gbps << " Gbps" << endl;
        cout << "  Total WAN capacity: " << total_wan_capacity_gbps << " Gbps" << endl;
        cout << "  Overall WAN utilization: " << overall_wan_utilization << "%" << endl;
        
        cout << "\n  Congestion Analysis:" << endl;
        if (overall_wan_utilization > 90.0) {
            cout << "    ⚠⚠⚠ CRITICAL: WAN is severely congested!" << endl;
            cout << "    Recommendation: Increase WAN bandwidth or reduce traffic load" << endl;
        } else if (overall_wan_utilization > 70.0) {
            cout << "    ⚠ WARNING: WAN is approaching capacity" << endl;
            cout << "    Recommendation: Monitor closely and consider capacity expansion" << endl;
        } else if (overall_wan_utilization > 50.0) {
            cout << "    ✓ Moderate: WAN is handling traffic well" << endl;
        } else {
            cout << "    ✓ Good: WAN has plenty of available capacity" << endl;
        }
        
        // Compare inter-DC traffic with available WAN bandwidth
        cout << "\n  Inter-DC Traffic Analysis:" << endl;
        cout << "    Number of inter-DC flows: " << num_inter_dc_flows << endl;
        if (num_inter_dc_flows > 0) {
            double avg_wan_throughput_per_flow = total_wan_throughput_gbps / num_inter_dc_flows;
            cout << "    Average WAN throughput per inter-DC flow: " << avg_wan_throughput_per_flow << " Gbps" << endl;
            cout << "    Expected per-flow rate: " << (inter_dc_flow_rate * packet_size * 8 / 1e9) << " Gbps" << endl;
        }
    } else {
        cout << "  No WAN traffic recorded - this may indicate no inter-DC communication" << endl;
    }
    
    cout << "\n**********************************************\n" << endl;
    
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