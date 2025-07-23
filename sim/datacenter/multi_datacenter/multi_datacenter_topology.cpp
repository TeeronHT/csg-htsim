#include "multi_datacenter_topology.h"
#include "loggers.h"
#include "config.h"
#include "constant_cca_scheduler.h"
#include <iostream>
#include <sstream>

MultiDatacenterTopology::MultiDatacenterTopology(uint32_t num_datacenters, uint32_t nodes_per_dc, 
                                                 linkspeed_bps intra_dc_speed, linkspeed_bps wan_speed,
                                                 mem_b intra_dc_queue_size, mem_b wan_queue_size,
                                                 simtime_picosec wan_delay, QueueLoggerFactory* logger_factory,
                                                 EventList* eventlist, queue_type qt)
    : _num_datacenters(num_datacenters), _nodes_per_dc(nodes_per_dc), _total_nodes(num_datacenters * nodes_per_dc),
      _intra_dc_speed(intra_dc_speed), _wan_speed(wan_speed), _intra_dc_queue_size(intra_dc_queue_size),
      _wan_queue_size(wan_queue_size), _wan_delay(wan_delay), _queue_type(qt),
      _logger_factory(logger_factory), _eventlist(eventlist) {
    
    std::cout << "Creating Multi-Datacenter Topology with " << num_datacenters 
              << " datacenters, " << nodes_per_dc << " nodes per DC" << std::endl;
    
    init_datacenters();
    init_wan_connections();
    setup_wan_routing();
}

MultiDatacenterTopology::~MultiDatacenterTopology() {
    std::cout << "  [Destructor] Deleting datacenters..." << std::endl;
    for (size_t i = 0; i < _datacenters.size(); ++i) {
        std::cout << "    Deleting datacenter " << i << std::endl;
        delete _datacenters[i];
        std::cout << "    Deleted datacenter " << i << std::endl;
    }
    std::cout << "  [Destructor] Deleting WAN switches..." << std::endl;
    for (size_t i = 0; i < _wan_switches.size(); ++i) {
        std::cout << "    Deleting WAN switch " << i << std::endl;
        delete _wan_switches[i];
        std::cout << "    Deleted WAN switch " << i << std::endl;
    }
    std::cout << "  [Destructor] Done." << std::endl;
}

void MultiDatacenterTopology::init_datacenters() {
    _datacenters.resize(_num_datacenters);
    _wan_switches.resize(_num_datacenters);
    
    // Set FatTreeTopology tier parameters BEFORE creating any datacenters
    // For k=8 fat tree with 128 hosts per datacenter
    std::cout << "Setting FatTreeTopology tier parameters for k=8..." << std::endl;
    
    // Set hosts_per_pod to enable custom mode (k=8, so hosts_per_pod = k²/4 = 16)
    FatTreeTopology::set_podsize(16); // Enable custom mode
    FatTreeTopology::set_tier_parameters(0, 4, 4, _intra_dc_queue_size, _intra_dc_queue_size, 1, _intra_dc_speed, 1); // TOR tier (k/2=4)
    FatTreeTopology::set_tier_parameters(1, 4, 4, _intra_dc_queue_size, _intra_dc_queue_size, 1, _intra_dc_speed, 1); // AGG tier (k/2=4)
    FatTreeTopology::set_tier_parameters(2, 0, 8, 0, _intra_dc_queue_size, 1, _intra_dc_speed, 1); // CORE tier (k=8)
    
    for (uint32_t dc_id = 0; dc_id < _num_datacenters; dc_id++) {
        std::cout << "Creating DC " << dc_id << "..." << std::endl;
        
        // Create individual fat tree topology for this datacenter
        std::cout << "  Creating FatTreeTopology for DC " << dc_id << "..." << std::endl;
        _datacenters[dc_id] = new FatTreeTopology(_nodes_per_dc, _intra_dc_speed, _intra_dc_queue_size,
                                                  _logger_factory, _eventlist, nullptr, _queue_type, 
                                                  0, 0, CONST_SCHEDULER);
        std::cout << "  FatTreeTopology created for DC " << dc_id << std::endl;
        
        // Set host offset for this datacenter
        uint32_t host_offset = dc_id * _nodes_per_dc;
        _datacenters[dc_id]->set_host_offset(host_offset);
        std::cout << "  Set host offset for DC " << dc_id << " to " << host_offset << std::endl;
        
        // Debug: Check the topology configuration
        std::cout << "  DC " << dc_id << " has " << _datacenters[dc_id]->no_of_nodes() << " nodes" << std::endl;
        
        // Create WAN switch for this datacenter
        std::cout << "  Creating WAN switch for DC " << dc_id << "..." << std::endl;
        std::stringstream ss;
        ss << "WAN_Switch_DC" << dc_id;
        // Pass nullptr as FatTree topology to decouple WAN switch from local routing
        _wan_switches[dc_id] = new FatTreeSwitch(*_eventlist, ss.str(), FatTreeSwitch::WAN, dc_id, 
                                                 timeFromUs((uint32_t)1), nullptr);
        
        // Configure WAN switch for multi-DC
        _wan_switches[dc_id]->set_dc_id(dc_id);
        _wan_switches[dc_id]->set_total_dcs(_num_datacenters);
        _wan_switches[dc_id]->set_nodes_per_dc(_nodes_per_dc);
        
        // Configure all switches in the FatTree topology for multi-DC
        // This is needed so they can properly identify inter-DC traffic
        _datacenters[dc_id]->set_multi_dc_info(dc_id, _num_datacenters, _nodes_per_dc);
        
        // Connect WAN switch to the FatTree topology
        // This allows CORE switches to route inter-DC traffic to WAN switches
        _datacenters[dc_id]->connect_wan_switch(_wan_switches[dc_id]);
        
        std::cout << "Created DC " << dc_id << " with " << _nodes_per_dc << " nodes" << std::endl;
    }
}

void MultiDatacenterTopology::init_wan_connections() {
    // Initialize WAN pipes and queues between all datacenter pairs
    _wan_pipes.resize(_num_datacenters);
    _wan_queues.resize(_num_datacenters);
    
    for (uint32_t src_dc = 0; src_dc < _num_datacenters; src_dc++) {
        _wan_pipes[src_dc].resize(_num_datacenters);
        _wan_queues[src_dc].resize(_num_datacenters);
        
        for (uint32_t dest_dc = 0; dest_dc < _num_datacenters; dest_dc++) {
            if (src_dc != dest_dc) {
                // Create WAN connection from src_dc to dest_dc
                std::stringstream ss;
                ss << "WAN_Pipe_DC" << src_dc << "_to_DC" << dest_dc;
                _wan_pipes[src_dc][dest_dc] = new Pipe(_wan_delay, *_eventlist);
                _wan_pipes[src_dc][dest_dc]->setName(ss.str());
                
                ss.str("");
                ss << "WAN_Queue_DC" << src_dc << "_to_DC" << dest_dc;
                _wan_queues[src_dc][dest_dc] = new RandomQueue(_wan_speed, _wan_queue_size, *_eventlist, nullptr, _wan_queue_size);
                _wan_queues[src_dc][dest_dc]->setName(ss.str());
                
                std::cout << "Created WAN connection from DC " << src_dc << " to DC " << dest_dc << std::endl;
            }
        }
    }
}

void MultiDatacenterTopology::setup_wan_routing() {
    // Set up routing tables for WAN switches
    for (uint32_t dc_id = 0; dc_id < _num_datacenters; dc_id++) {
        FatTreeSwitch* wan_switch = _wan_switches[dc_id];
        
        for (uint32_t dest_dc = 0; dest_dc < _num_datacenters; dest_dc++) {
            if (dc_id != dest_dc) {
                // Add routes from this WAN switch to all hosts in the destination DC
                for (uint32_t host_in_dest_dc = 0; host_in_dest_dc < _nodes_per_dc; host_in_dest_dc++) {
                    uint32_t global_host_id = dest_dc * _nodes_per_dc + host_in_dest_dc;
                    
                    Route* route = new Route();
                    route->push_back(_wan_queues[dc_id][dest_dc]);
                    route->push_back(_wan_pipes[dc_id][dest_dc]);
                    route->push_back(_wan_switches[dest_dc]);
                    
                    // Add the route to the WAN switch's FIB
                    wan_switch->add_wan_route(global_host_id, route);
                    std::cout << "Created WAN route from DC" << dc_id << " to host " << global_host_id << " in DC" << dest_dc << std::endl;
                    
                    // Verify the route was added
                    if (host_in_dest_dc % 10 == 0) { // Only check every 10th route to avoid spam
                        std::cout << "  Verified WAN route for host " << global_host_id << " has " << route->size() << " elements" << std::endl;
                    }
                }
            }
        }
    }
}

vector<const Route*>* MultiDatacenterTopology::get_bidir_paths(uint32_t src, uint32_t dest, bool reverse) {
    vector<const Route*>* paths = new vector<const Route*>();
    
    uint32_t src_dc = get_dc_id(src);
    uint32_t dest_dc = get_dc_id(dest);
    uint32_t src_local = get_local_host_id(src);
    uint32_t dest_local = get_local_host_id(dest);
    
    std::cout << "get_bidir_paths: src=" << src << " dest=" << dest 
              << " (DC" << src_dc << ":" << src_local << " -> DC" << dest_dc << ":" << dest_local << ")" << std::endl;
    std::cout << "  DC" << src_dc << " has " << _datacenters[src_dc]->no_of_nodes() << " nodes" << std::endl;
    
    if (src_dc == dest_dc) {
        // Intra-DC routing - use the local fat tree topology
        std::cout << "  Intra-DC routing" << std::endl;
        std::cout << "  Local host IDs: src=" << src_local << " dest=" << dest_local << std::endl;
        
        // Validate datacenter access
        if (src_dc >= _datacenters.size()) {
            std::cerr << "ERROR: Invalid datacenter ID " << src_dc << " (max: " << _datacenters.size() - 1 << ")" << std::endl;
            return paths;
        }
        
        if (!_datacenters[src_dc]) {
            std::cerr << "ERROR: Datacenter " << src_dc << " is null" << std::endl;
            return paths;
        }
        
        vector<const Route*>* local_paths = _datacenters[src_dc]->get_bidir_paths(src, dest, reverse);
        std::cout << "  Got " << local_paths->size() << " paths" << std::endl;
        
        if (local_paths->empty()) {
            std::cerr << "ERROR: No paths found from host " << src_local << " to " << dest_local << " in DC " << src_dc << std::endl;
            std::cerr << "  Total nodes in DC: " << _datacenters[src_dc]->no_of_nodes() << std::endl;
        }
        
        // Copy the paths from the local datacenter
        for (size_t i = 0; i < local_paths->size(); i++) {
            paths->push_back((*local_paths)[i]);
        }
    } else {
        // Inter-DC routing - need to go through WAN
        std::cout << "  Inter-DC routing - using WAN" << std::endl;
        
        // Get the WAN route
        Route* wan_route = get_wan_route(src_dc, dest_dc, src, dest);
        if (wan_route) {
            paths->push_back(wan_route);
            std::cout << "  Created WAN route with " << wan_route->size() << " elements" << std::endl;
        } else {
            std::cerr << "  Failed to create WAN route from DC" << src_dc << " host " << src_local 
                      << " to DC" << dest_dc << " host " << dest_local << std::endl;
        }
    }
    
    return paths;
}

Route* MultiDatacenterTopology::get_wan_route(uint32_t src_dc, uint32_t dest_dc, uint32_t src_host, uint32_t dest_host) {
    // Check cache first
    auto key = std::make_pair(src_host, dest_host);
    if (_wan_routes_cache.find(key) != _wan_routes_cache.end()) {
        return _wan_routes_cache[key];
    }
    
    // Ensure this is only called for inter-DC routing
    if (src_dc == dest_dc) {
        std::cerr << "ERROR: get_wan_route called for intra-DC routing (DC" << src_dc << " to DC" << dest_dc << ")" << std::endl;
        return nullptr;
    }
    
    std::cout << "Building WAN route from DC" << src_dc << " host " << src_host 
              << " to DC" << dest_dc << " host " << dest_host << std::endl;
    
    // Validate that WAN connections exist
    if (src_dc >= _wan_queues.size() || dest_dc >= _wan_queues[src_dc].size() || 
        !_wan_queues[src_dc][dest_dc] || !_wan_pipes[src_dc][dest_dc] || !_wan_switches[dest_dc]) {
        std::cerr << "ERROR: WAN connection not properly initialized for DC" << src_dc << " to DC" << dest_dc << std::endl;
        return nullptr;
    }
    
    Route* complete_route = new Route();
    
    // Add a scheduler at the beginning (required by ConstantErasureCcaSrc::connect)
    ConstFairScheduler* scheduler = new ConstFairScheduler(_intra_dc_speed, *_eventlist, nullptr);
    complete_route->push_back(scheduler);
    
    // Add WAN connection - only for inter-DC traffic
    complete_route->push_back(_wan_queues[src_dc][dest_dc]);
    complete_route->push_back(_wan_pipes[src_dc][dest_dc]);
    complete_route->push_back(_wan_switches[dest_dc]);
    
    // Cache the route
    _wan_routes_cache[key] = complete_route;
    
    std::cout << "Created WAN route with " << complete_route->size() << " elements" << std::endl;
    
    return complete_route;
}

void MultiDatacenterTopology::add_host_port(uint32_t hostnum, flowid_t flow_id, PacketSink* host) {
    uint32_t dc_id = get_dc_id(hostnum);
    
    // Add to the appropriate datacenter using global host ID
    _datacenters[dc_id]->add_host_port(hostnum, flow_id, host);
}

uint32_t MultiDatacenterTopology::get_dc_id(uint32_t host_id) const {
    return host_id / _nodes_per_dc;
}

uint32_t MultiDatacenterTopology::get_local_host_id(uint32_t host_id) const {
    return host_id % _nodes_per_dc;
}

bool MultiDatacenterTopology::is_inter_dc_flow(uint32_t src, uint32_t dest) const {
    return get_dc_id(src) != get_dc_id(dest);
}

FatTreeTopology* MultiDatacenterTopology::get_datacenter(uint32_t dc_id) const {
    if (dc_id < _datacenters.size()) {
        return _datacenters[dc_id];
    }
    return nullptr;
}
