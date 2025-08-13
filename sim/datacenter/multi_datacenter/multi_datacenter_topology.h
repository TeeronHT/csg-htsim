#ifndef MULTI_DATACENTER_TOPOLOGY_H
#define MULTI_DATACENTER_TOPOLOGY_H

#include "fat_tree_topology.h"
#include "fat_tree_switch.h"
#include "queue.h"
#include "pipe.h"
#include "route.h"
#include <vector>
#include <map>

class MultiDatacenterTopology : public Topology {
public:
    MultiDatacenterTopology(uint32_t num_datacenters, uint32_t nodes_per_dc, 
                           linkspeed_bps intra_dc_speed, linkspeed_bps wan_speed,
                           mem_b intra_dc_queue_size, mem_b wan_queue_size,
                           simtime_picosec wan_delay, QueueLoggerFactory* logger_factory,
                           EventList* eventlist, queue_type qt);
    
    virtual ~MultiDatacenterTopology();
    
    // Override Topology methods
    virtual vector<const Route*>* get_bidir_paths(uint32_t src, uint32_t dest, bool reverse);
    virtual uint32_t no_of_nodes() const { return _total_nodes; }
    virtual void add_host_port(uint32_t hostnum, flowid_t flow_id, PacketSink* host);
    virtual vector<uint32_t>* get_neighbours(uint32_t src) { return nullptr; }
    
    // Multi-DC specific methods
    uint32_t get_dc_id(uint32_t host_id) const;
    uint32_t get_local_host_id(uint32_t host_id) const;
    bool is_inter_dc_flow(uint32_t src, uint32_t dest) const;
    
    // Get individual datacenter topologies
    FatTreeTopology* get_datacenter(uint32_t dc_id) const;
    
    // WAN routing methods
    Route* get_wan_route(uint32_t src_dc, uint32_t dest_dc, uint32_t src_host, uint32_t dest_host);
    
private:
    void init_datacenters();
    void init_wan_connections();
    void setup_wan_routing();
    void create_core_wan_connections(uint32_t dc_id);
    
    uint32_t _num_datacenters;
    uint32_t _nodes_per_dc;
    uint32_t _total_nodes;
    
    linkspeed_bps _intra_dc_speed;
    linkspeed_bps _wan_speed;
    mem_b _intra_dc_queue_size;
    mem_b _wan_queue_size;
    simtime_picosec _wan_delay;
    
    queue_type _queue_type;
    QueueLoggerFactory* _logger_factory;
    EventList* _eventlist;
    
    // Individual datacenter topologies
    std::vector<FatTreeTopology*> _datacenters;
    
    // WAN switches (one per datacenter)
    std::vector<FatTreeSwitch*> _wan_switches;
    
    // WAN connections between datacenters
    std::vector<std::vector<Pipe*>> _wan_pipes;  // [src_dc][dest_dc]
    std::vector<std::vector<Queue*>> _wan_queues; // [src_dc][dest_dc]
    
    // WAN routes cache
    std::map<std::pair<uint32_t, uint32_t>, Route*> _wan_routes_cache;
};

#endif // MULTI_DATACENTER_TOPOLOGY_H
