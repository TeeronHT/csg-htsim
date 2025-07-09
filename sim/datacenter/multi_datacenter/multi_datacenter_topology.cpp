#include "multi_datacenter_topology.h"
#include "loggers.h"
#include "config.h"
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
    for (auto dc : _datacenters) {
        delete dc;
    }
    for (auto ws : _wan_switches) {
        delete ws;
    }
}

void MultiDatacenterTopology::init_datacenters() {
    _datacenters.resize(_num_datacenters);
    _wan_switches.resize(_num_datacenters);
    
    for (uint32_t dc_id = 0; dc_id < _num_datacenters; dc_id++) {
        // Create individual fat tree topology for this datacenter
        _datacenters[dc_id] = new FatTreeTopology(_nodes_per_dc, _intra_dc_speed, _intra_dc_queue_size,
                                                  _logger_factory, _eventlist, nullptr, _queue_type);
        
        // Create WAN switch for this datacenter
        std::stringstream ss;
        ss << "WAN_Switch_DC" << dc_id;
        _wan_switches[dc_id] = new FatTreeSwitch(*_eventlist, ss.str(), FatTreeSwitch::WAN, dc_id, 
                                                 timeFromUs((uint32_t)1), _datacenters[dc_id]);
        
        // Configure WAN switch for multi-DC
        _wan_switches[dc_id]->set_dc_id(dc_id);
        _wan_switches[dc_id]->set_total_dcs(_num_datacenters);
        _wan_switches[dc_id]->set_nodes_per_dc(_nodes_per_dc);
        
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
                    
                    // Note: _fib is protected, we'll need to access it differently
                    // For now, we'll skip this routing setup and handle it in the switch logic
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
    
    if (src_dc == dest_dc) {
        // Intra-DC routing - use the local fat tree topology
        paths = _datacenters[src_dc]->get_bidir_paths(src_local, dest_local, reverse);
    } else {
        // Inter-DC routing - need to go through WAN
        Route* route = get_wan_route(src_dc, dest_dc, src, dest);
        if (route) {
            paths->push_back(route);
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
    
    // Build the route: src_host -> local_tor -> local_agg -> local_core -> local_wan -> remote_wan -> remote_core -> remote_agg -> remote_tor -> dest_host
    
    // Get route from src_host to local WAN switch
    vector<const Route*>* local_paths = _datacenters[src_dc]->get_bidir_paths(get_local_host_id(src_host), 0, false);
    if (local_paths->empty()) {
        std::cerr << "No local path found from host " << src_host << " in DC " << src_dc << std::endl;
        return nullptr;
    }
    
    // Get route from remote WAN switch to dest_host
    vector<const Route*>* remote_paths = _datacenters[dest_dc]->get_bidir_paths(0, get_local_host_id(dest_host), false);
    if (remote_paths->empty()) {
        std::cerr << "No remote path found to host " << dest_host << " in DC " << dest_dc << std::endl;
        return nullptr;
    }
    
    // Build complete route
    Route* complete_route = new Route();
    
    // Add local path (excluding the final destination)
    const Route* local_route = (*local_paths)[0];
    for (size_t i = 0; i < local_route->size() - 1; i++) {
        complete_route->push_back(local_route->at(i));
    }
    
    // Add WAN connection
    complete_route->push_back(_wan_queues[src_dc][dest_dc]);
    complete_route->push_back(_wan_pipes[src_dc][dest_dc]);
    complete_route->push_back(_wan_switches[dest_dc]);
    
    // Add remote path (excluding the initial source)
    const Route* remote_route = (*remote_paths)[0];
    for (size_t i = 1; i < remote_route->size(); i++) {
        complete_route->push_back(remote_route->at(i));
    }
    
    // Cache the route
    _wan_routes_cache[key] = complete_route;
    
    return complete_route;
}

void MultiDatacenterTopology::add_host_port(uint32_t hostnum, flowid_t flow_id, PacketSink* host) {
    uint32_t dc_id = get_dc_id(hostnum);
    uint32_t local_host_id = get_local_host_id(hostnum);
    
    // Add to the appropriate datacenter
    _datacenters[dc_id]->add_host_port(local_host_id, flow_id, host);
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
