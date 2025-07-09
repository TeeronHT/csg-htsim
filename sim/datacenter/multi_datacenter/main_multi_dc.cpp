#include <iostream>
#include "eventlist.h"
#include "logfile.h"
#include "fat_tree_topology.h"
#include "queue.h"
#include "pipe.h"
#include "route.h"
#include "constant_erase.h"
#include "loggers.h"

int main() {
    // Setup eventlist and logging
    EventList eventlist;
    eventlist.setEndtime(timeFromSec(1)); // simulate 1 second

    Logfile logfile("log.txt");
    QueueLoggerSimple* qlogger = new QueueLoggerSimple();
    logfile.addLogger(*qlogger);

    // Parameters
    uint32_t nodes_per_dc = 100;
    linkspeed_bps intra_dc_speed = speedFromGbps(10);
    linkspeed_bps wan_speed = speedFromGbps(1);
    mem_b queue_size = memFromPkt(1000);
    mem_b wan_queue_size = memFromPkt(5000);
    simtime_picosec wan_delay = timeFromMs(20); // 20ms latency

    // Create two datacenters
    FatTreeTopology* dc1 = new FatTreeTopology(nodes_per_dc, intra_dc_speed, queue_size, qlogger, &eventlist, nullptr, ECN);
    FatTreeTopology* dc2 = new FatTreeTopology(nodes_per_dc, intra_dc_speed, queue_size, qlogger, &eventlist, nullptr, ECN);
    uint32_t offset_dc2 = nodes_per_dc;

    // Connect datacenters with WAN
    Pipe* wan_pipe_12 = new Pipe(wan_delay, &eventlist);
    Pipe* wan_pipe_21 = new Pipe(wan_delay, &eventlist);
    Queue* wan_queue_12 = new Queue(wan_queue_size, wan_speed, qlogger, &eventlist, "WAN_DC1_to_DC2");
    Queue* wan_queue_21 = new Queue(wan_queue_size, wan_speed, qlogger, &eventlist, "WAN_DC2_to_DC1");

    // Pick two nodes (one in each DC)
    uint32_t src = 0;            // host 0 in DC1
    uint32_t dst = offset_dc2;   // host 0 in DC2

    // Get internal paths
    const Route* part1 = dc1->get_bidir_paths(src, nodes_per_dc - 1, false)->at(0);
    const Route* part2 = dc2->get_bidir_paths(0, 0, false)->at(0);

    // Build route from src to dst
    Route* forward_route = new Route();
    for (auto& hop : *part1) forward_route->push_back(hop);
    forward_route->push_back(wan_queue_12);
    forward_route->push_back(wan_pipe_12);
    for (auto& hop : *part2) forward_route->push_back(hop);

    // Create sink
    PacketSink* sink = new PacketSink();
    sink->setName("Sink");
    logfile.writeName(*sink);

    // Connect sink
    forward_route->push_back(sink);

    // Create source
    ConstantErasureCca* source = new ConstantErasureCca(forward_route, &eventlist, nullptr);
    source->setName("CE-Source");
    logfile.writeName(*source);
    source->setRate(wan_speed); // match WAN bandwidth
    source->start(timeFromMs(0));

    // Run simulation
    while (eventlist.doNextEvent()) {}

    return 0;
}

