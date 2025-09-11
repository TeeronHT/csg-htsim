// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "multi_fat_tree_switch.h"
#include "routetable.h"
#include "multi_fat_tree_topology.h"
#include "fat_tree_switch.h"  // For FlowletInfo definition
#include "callback_pipe.h"
#include "queue_lossless.h"
#include "queue_lossless_output.h"
#include "constant_cca_packet.h"

unordered_map<BaseQueue*,uint32_t> MultiFatTreeSwitch::_port_flow_counts;

MultiFatTreeSwitch::MultiFatTreeSwitch(EventList& eventlist, string s, switch_type t, uint32_t id,simtime_picosec delay, MultiFatTreeTopology* ft): Switch(eventlist, s) {
    _id = id;
    _type = t;
    _pipe = new CallbackPipe(delay,eventlist, this);
    _uproutes = NULL;
    _ft = ft;
    _crt_route = 0;
    _hash_salt = random();
    _last_choice = eventlist.now();
    _fib = new RouteTable();
    
    // Initialize multi-DC fields
    _dc_id = 0;
    _total_dcs = 1;
    _nodes_per_dc = 0;
}

void MultiFatTreeSwitch::receivePacket(Packet& pkt){
    if (pkt.type()==ETH_PAUSE){
        EthPausePacket* p = (EthPausePacket*)&pkt;
        //I must be in lossless mode!
        //find the egress queue that should process this, and pass it over for processing. 
        for (size_t i = 0;i < _ports.size();i++){
            LosslessQueue* q = (LosslessQueue*)_ports.at(i);
            if (q->getRemoteEndpoint() && ((Switch*)q->getRemoteEndpoint())->getID() == p->senderID()){
                q->receivePacket(pkt);
                break;
            }
        }
        
        return;
    }

    if (_packets.find(&pkt)==_packets.end()){
        //ingress pipeline processing.

        _packets[&pkt] = true;

        const Route * nh = getNextHop(pkt,NULL);
        //set next hop which is peer switch.
        pkt.set_route(*nh);

        //emulate the switching latency between ingress and packet arriving at the egress queue.
        _pipe->receivePacket(pkt); 
    }
    else {
        _packets.erase(&pkt);
        
        //egress queue processing.
        //cout << "Switch type " << _type <<  " id " << _id << " pkt dst " << pkt.dst() << " dir " << pkt.get_direction() << endl;
        pkt.sendOn();
    }
};

void MultiFatTreeSwitch::addHostPort(int addr, int flowid, PacketSink* transport){
    // WAN switches don't have a FatTree topology and don't need host ports
    if (_type == WAN) {
        // WAN switches are standalone and don't connect directly to hosts
        return;
    }
    
    // Only TOR switches should have host ports
    if (_type != TOR) {
        cerr << "Warning: Attempting to add host port to non-TOR switch (type " << _type << ")" << endl;
        return;
    }
    
    Route* rt = new Route();
    // Convert global host ID to local host ID for accessing topology arrays
    uint32_t local_addr = _ft->adjusted_host(addr);
    // Use local host ID to calculate switch ID for array access
    uint32_t switch_id = _ft->HOST_POD_SWITCH(local_addr);
    rt->push_back(_ft->queues_nlp_ns[switch_id][local_addr][0]);
    rt->push_back(_ft->pipes_nlp_ns[switch_id][local_addr][0]);
    rt->push_back(transport);
    _fib->addHostRoute(addr,rt,flowid);
}

static uint32_t mhash(uint32_t x) {
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

uint32_t MultiFatTreeSwitch::adaptive_route_p2c(vector<FibEntry*>* ecmp_set, int8_t (*cmp)(FibEntry*,FibEntry*)){
    uint32_t choice = 0, min = UINT32_MAX;
    uint32_t start, i = 0;
    static const uint16_t nr_choices = 2;
    
    do {
        start = random()%ecmp_set->size();

        Route * r= (*ecmp_set)[start]->getEgressPort();
        assert(r && r->size()>1);
        BaseQueue* q = (BaseQueue*)(r->at(0));
        assert(q);
        if (q->queuesize()<min){
            choice = start;
            min = q->queuesize();
        }
        i++;
    } while (i<nr_choices);
    return choice;
}

uint32_t MultiFatTreeSwitch::adaptive_route(vector<FibEntry*>* ecmp_set, int8_t (*cmp)(FibEntry*,FibEntry*)){
    //cout << "adaptive_route" << endl;
    uint32_t choice = 0;

    uint32_t best_choices[256];
    uint32_t best_choices_count = 0;
  
    FibEntry* min = (*ecmp_set)[choice];
    best_choices[best_choices_count++] = choice;

    for (uint32_t i = 1; i< ecmp_set->size(); i++){
        int8_t c = cmp(min,(*ecmp_set)[i]);

        if (c < 0){
            choice = i;
            min = (*ecmp_set)[choice];
            best_choices_count = 0;
            best_choices[best_choices_count++] = choice;
        }
        else if (c==0){
            assert(best_choices_count<255);
            best_choices[best_choices_count++] = i;
        }        
    }

    assert (best_choices_count>=1);
    uint32_t choiceindex = random()%best_choices_count;
    choice = best_choices[choiceindex];
    //cout << "ECMP set choices " << ecmp_set->size() << " Choice count " << best_choices_count << " chosen entry " << choiceindex << " chosen path " << choice << " ";

    if (cmp==compare_flow_count){
        //for (uint32_t i = 0; i<best_choices_count;i++)
          //  cout << "pathcnt " << best_choices[i] << "="<< _port_flow_counts[(BaseQueue*)( (*ecmp_set)[best_choices[i]]->getEgressPort()->at(0))]<< " ";
        
        _port_flow_counts[(BaseQueue*)((*ecmp_set)[choice]->getEgressPort()->at(0))]++;
    }

    return choice;
}

uint32_t MultiFatTreeSwitch::replace_worst_choice(vector<FibEntry*>* ecmp_set, int8_t (*cmp)(FibEntry*,FibEntry*),uint32_t my_choice){
    uint32_t best_choice = 0;
    uint32_t worst_choice = 0;

    uint32_t best_choices[256];
    uint32_t best_choices_count = 0;

    FibEntry* min = (*ecmp_set)[best_choice];
    FibEntry* max = (*ecmp_set)[worst_choice];
    best_choices[best_choices_count++] = best_choice;

    for (uint32_t i = 1; i< ecmp_set->size(); i++){
        int8_t c = cmp(min,(*ecmp_set)[i]);

        if (c < 0){
            best_choice = i;
            min = (*ecmp_set)[best_choice];
            best_choices_count = 0;
            best_choices[best_choices_count++] = best_choice;
        }
        else if (c==0){
            assert(best_choices_count<256);
            best_choices[best_choices_count++] = i;
        }        

        if (cmp(max,(*ecmp_set)[i])>0){
            worst_choice = i;
            max = (*ecmp_set)[worst_choice];
        }
    }

    //might need to play with different alternatives here, compare to worst rather than just to worst index.
    int8_t r = cmp((*ecmp_set)[my_choice],(*ecmp_set)[worst_choice]);
    assert(r>=0);

    if (r==0){
        assert (best_choices_count>=1);
        return best_choices[random()%best_choices_count];
    }
    else return my_choice;
}


int8_t MultiFatTreeSwitch::compare_pause(FibEntry* left, FibEntry* right){
    Route * r1= left->getEgressPort();
    assert(r1 && r1->size()>1);
    LosslessOutputQueue* q1 = dynamic_cast<LosslessOutputQueue*>(r1->at(0));
    Route * r2= right->getEgressPort();
    assert(r2 && r2->size()>1);
    LosslessOutputQueue* q2 = dynamic_cast<LosslessOutputQueue*>(r2->at(0));

    if (!q1->is_paused()&&q2->is_paused())
        return 1;
    else if (q1->is_paused()&&!q2->is_paused())
        return -1;
    else 
        return 0;
}

int8_t MultiFatTreeSwitch::compare_flow_count(FibEntry* left, FibEntry* right){
    Route * r1= left->getEgressPort();
    assert(r1 && r1->size()>1);
    BaseQueue* q1 = (BaseQueue*)(r1->at(0));
    Route * r2= right->getEgressPort();
    assert(r2 && r2->size()>1);
    BaseQueue* q2 = (BaseQueue*)(r2->at(0));

    if (_port_flow_counts.find(q1)==_port_flow_counts.end())
        _port_flow_counts[q1] = 0;

    if (_port_flow_counts.find(q2)==_port_flow_counts.end())
        _port_flow_counts[q2] = 0;

    //cout << "CMP q1 " << q1 << "=" << _port_flow_counts[q1] << " q2 " << q2 << "=" << _port_flow_counts[q2] << endl; 

    if (_port_flow_counts[q1] < _port_flow_counts[q2])
        return 1;
    else if (_port_flow_counts[q1] > _port_flow_counts[q2] )
        return -1;
    else 
        return 0;
}

int8_t MultiFatTreeSwitch::compare_queuesize(FibEntry* left, FibEntry* right){
    Route * r1= left->getEgressPort();
    assert(r1 && r1->size()>1);
    BaseQueue* q1 = dynamic_cast<BaseQueue*>(r1->at(0));
    Route * r2= right->getEgressPort();
    assert(r2 && r2->size()>1);
    BaseQueue* q2 = dynamic_cast<BaseQueue*>(r2->at(0));

    if (q1->quantized_queuesize() < q2->quantized_queuesize())
        return 1;
    else if (q1->quantized_queuesize() > q2->quantized_queuesize())
        return -1;
    else 
        return 0;
}

int8_t MultiFatTreeSwitch::compare_bandwidth(FibEntry* left, FibEntry* right){
    Route * r1= left->getEgressPort();
    assert(r1 && r1->size()>1);
    BaseQueue* q1 = dynamic_cast<BaseQueue*>(r1->at(0));
    Route * r2= right->getEgressPort();
    assert(r2 && r2->size()>1);
    BaseQueue* q2 = dynamic_cast<BaseQueue*>(r2->at(0));

    if (q1->quantized_utilization() < q2->quantized_utilization())
        return 1;
    else if (q1->quantized_utilization() > q2->quantized_utilization())
        return -1;
    else 
        return 0;

    /*if (q1->average_utilization() < q2->average_utilization())
        return 1;
    else if (q1->average_utilization() > q2->average_utilization())
        return -1;
    else 
        return 0;        */
}

int8_t MultiFatTreeSwitch::compare_pqb(FibEntry* left, FibEntry* right){
    //compare pause, queuesize, bandwidth.
    int8_t p = compare_pause(left, right);

    if (p!=0)
        return p;
    
    p = compare_queuesize(left,right);

    if (p!=0)
        return p;

    return compare_bandwidth(left,right);
}

int8_t MultiFatTreeSwitch::compare_pq(FibEntry* left, FibEntry* right){
    //compare pause, queuesize, bandwidth.
    int8_t p = compare_pause(left, right);

    if (p!=0)
        return p;
    
    return compare_queuesize(left,right);
}

int8_t MultiFatTreeSwitch::compare_qb(FibEntry* left, FibEntry* right){
    //compare pause, queuesize, bandwidth.
    int8_t p = compare_queuesize(left, right);

    if (p!=0)
        return p;
    
    return compare_bandwidth(left,right);
}

int8_t MultiFatTreeSwitch::compare_pb(FibEntry* left, FibEntry* right){
    //compare pause, queuesize, bandwidth.
    int8_t p = compare_pause(left, right);

    if (p!=0)
        return p;
    
    return compare_bandwidth(left,right);
}

void MultiFatTreeSwitch::permute_paths(vector<FibEntry *>* uproutes) {
    int len = uproutes->size();
    for (int i = 0; i < len; i++) {
        int ix = random() % (len - i);
        FibEntry* tmppath = (*uproutes)[ix];
        (*uproutes)[ix] = (*uproutes)[len-1-i];
        (*uproutes)[len-1-i] = tmppath;
    }
}

MultiFatTreeSwitch::routing_strategy MultiFatTreeSwitch::_strategy = MultiFatTreeSwitch::NIX;
uint16_t MultiFatTreeSwitch::_ar_fraction = 0;
uint16_t MultiFatTreeSwitch::_ar_sticky = MultiFatTreeSwitch::PER_PACKET;
simtime_picosec MultiFatTreeSwitch::_sticky_delta = timeFromUs((uint32_t)10);
double MultiFatTreeSwitch::_ecn_threshold_fraction = 0.5;
double MultiFatTreeSwitch::_speculative_threshold_fraction = 0.2;
int8_t (*MultiFatTreeSwitch::fn)(FibEntry*,FibEntry*)= &MultiFatTreeSwitch::compare_queuesize;

Route* MultiFatTreeSwitch::getNextHop(Packet& pkt, BaseQueue* ingress_port){

    // Need to account for local vs global host IDs

    // Handle WAN switches that don't have a FatTree topology
    if (_type == WAN) {
        // WAN switch routing logic
        uint32_t dst = pkt.dst();
        uint32_t src = pkt.src();

        // cerr << "Sequence number " << pkt.id();
        
        // cerr << "  WAN switch " << _id << " received packet: src=" << src << " dst=" << dst << " (from switch " << (ingress_port ? ingress_port->getSwitch()->getID() : -1) << ")" << endl;
        // cerr << "  WAN switch " << _id << " is connected to FatTree topology with host offset: " << (_ft ? _ft->_host_id_offset : -1) << endl;
        // cerr << "  WAN switch DC ID: " << _dc_id << ", dest host DC: " << (dst / _nodes_per_dc) << endl;
        // cerr << "  _nodes_per_dc: " << _nodes_per_dc << ", dst/_nodes_per_dc: " << (dst / _nodes_per_dc) << endl;
        // cerr << "  Is local host? " << ((dst / _nodes_per_dc == _dc_id) ? "YES" : "NO") << endl;
        // cerr << "  Debug: dst=" << dst << ", _dc_id=" << _dc_id << ", dst/_nodes_per_dc=" << (dst / _nodes_per_dc) << ", comparison=" << (dst / _nodes_per_dc == _dc_id) << endl;
        // cerr << "  Debug: WAN switch " << _id << " configuration: _dc_id=" << _dc_id << ", _nodes_per_dc=" << _nodes_per_dc << endl;
        
        // Case 1: Packet received from WAN link, need to send down to CORE router
        // This happens when the destination is in this DC (_dc_id == (dst / _nodes_per_dc))
        if (dst / _nodes_per_dc == _dc_id) {
            // cerr << "WAN switch " << _id << " routing packet to local DC host " << dst << endl;
            // cerr << "  WAN switch DC ID: " << _dc_id << ", dest host DC: " << (dst / _nodes_per_dc) << endl;
            
            // Get route to the destination through the local FatTree
            vector<FibEntry*>* available_hops = _fib->getRoutes(dst);
            if (!available_hops || available_hops->empty()) {
                cerr << "  ERROR: WAN switch no route found for local host " << dst << endl;
                // cerr << "  This should not happen - routes should be pre-populated" << endl;
                // cerr << "  Debug: Looking for host " << dst << " in WAN switch " << _id << " FIB" << endl;
                // cerr << "  Debug: WAN switch " << _id << " has DC ID " << _dc_id << " and nodes_per_dc " << _nodes_per_dc << endl;
                // cerr << "  Debug: Expected local host range: " << (_dc_id * _nodes_per_dc) << " to " << ((_dc_id + 1) * _nodes_per_dc - 1) << endl;
                return nullptr;
            }
            
            // cerr << "  SUCCESS: WAN switch " << _id << " found " << available_hops->size() << " routes for local host " << dst << endl;
            
            // ECMP path to destination host
            uint32_t ecmp_choice = 0;
            if (available_hops->size() > 1) {
                ecmp_choice = freeBSDHash(pkt.flow_id(), pkt.pathid(), _hash_salt) % available_hops->size();
            }
            
            FibEntry* e = (*available_hops)[ecmp_choice];
            pkt.set_direction(DOWN);
            return e->getEgressPort();
        }
        
        // Case 2: Packet needs to be sent over WAN link to different DC
        // This happens when _dc_id != (dst / _nodes_per_dc)
        uint32_t dest_dc = dst / _nodes_per_dc;
        // cerr << "WAN switch " << _id << " routing packet to remote DC host " << dst << " (DC" << dest_dc << ")" << endl;
        // cerr << "  WAN switch DC ID: " << _dc_id << ", dest host DC: " << dest_dc << endl;
        
        // Check FIB for cached WAN route
        vector<FibEntry*>* available_hops = _fib->getRoutes(dst);
        
        if (!available_hops || available_hops->empty()) {
            cerr << "  ERROR: WAN switch no route found for remote host " << dst << endl;
            cerr << "  Available routes in FIB: " << _fib->getRoutes(dst) << endl;
            return nullptr;
        }
        
        // cerr << "  Found " << available_hops->size() << " routes for host " << dst << endl;
        
        // Debug: Check what the route leads to
        // FibEntry* debug_e = (*available_hops)[0];
        // Route* debug_route = debug_e->getEgressPort();
        // cerr << "  DEBUG: WAN switch " << _id << " has route with " << (debug_route ? debug_route->size() : 0) << " elements" << endl;
        // if (debug_route && debug_route->size() > 0) {
        //     cerr << "  DEBUG: WAN switch " << _id << " first route element type: " << typeid(*debug_route->at(0)).name() << endl;
        // }
        
        // ECMP path to destination host
        uint32_t ecmp_choice = 0;
        if (available_hops->size() > 1) {
            ecmp_choice = freeBSDHash(pkt.flow_id(), pkt.pathid(), _hash_salt) % available_hops->size();
        }
        
        FibEntry* e = (*available_hops)[ecmp_choice];
        pkt.set_direction(DOWN);
        return e->getEgressPort();
    }
    
    // For non-WAN switches, ensure we have a FatTree topology
    if (!_ft) {
        cerr << "ERROR: Non-WAN switch " << _id << " has no FatTree topology" << endl;
        return nullptr;
    }
    
    vector<FibEntry*> * available_hops = _fib->getRoutes(pkt.dst());
    
    if (available_hops){
        //implement a form of ECMP hashing; might need to revisit based on measured performance.
        uint32_t ecmp_choice = 0;
        if (available_hops->size()>1)
            switch(_strategy){
            case NIX:
                abort();
            case ECMP:
                ecmp_choice = freeBSDHash(pkt.flow_id(),pkt.pathid(),_hash_salt) % available_hops->size();
                break;
            case ADAPTIVE_ROUTING:
                if (_ar_sticky==MultiFatTreeSwitch::PER_PACKET){
                    ecmp_choice = adaptive_route(available_hops,fn); 
                } 
                else if (_ar_sticky==MultiFatTreeSwitch::PER_FLOWLET){     
                    if (_flowlet_maps.find(pkt.flow_id())!=_flowlet_maps.end()){
                        FlowletInfo* f = _flowlet_maps[pkt.flow_id()];
                        
                        // only reroute an existing flow if its inter packet time is larger than _sticky_delta and
                        // and
                        // 50% chance happens. 
                        // and (commented out) if the switch has not taken any other placement decision that we've not seen the effects of.
                        if (eventlist().now() - f->_last > _sticky_delta && /*eventlist().now() - _last_choice > _pipe->delay() + BaseQueue::_update_period  &&*/ random()%2==0){ 
                            //cout << "AR 1 " << timeAsUs(eventlist().now()) << endl;
                            uint32_t new_route = adaptive_route(available_hops,fn); 
                            if (fn(available_hops->at(f->_egress),available_hops->at(new_route)) < 0){
                                f->_egress = new_route;
                                _last_choice = eventlist().now();
                                //cout << "Switch " << _type << ":" << _id << " choosing new path "<<  f->_egress << " for " << pkt.flow_id() << " at " << timeAsUs(eventlist().now()) << " last is " << timeAsUs(f->_last) << endl;
                            }
                        }
                        ecmp_choice = f->_egress;

                        f->_last = eventlist().now();
                    }
                    else {
                        //cout << "AR 2 " << timeAsUs(eventlist().now()) << endl;
                        ecmp_choice = adaptive_route(available_hops,fn); 
                        _last_choice = eventlist().now();

                        _flowlet_maps[pkt.flow_id()] = new FlowletInfo(ecmp_choice,eventlist().now());
                    }
                }

                break;
            case ECMP_ADAPTIVE:
                ecmp_choice = freeBSDHash(pkt.flow_id(),pkt.pathid(),_hash_salt) % available_hops->size();
                if (random()%100 < 50)
                    ecmp_choice = replace_worst_choice(available_hops,fn, ecmp_choice);
                break;
            case RR:
                if (_crt_route>=5 * available_hops->size()){
                    _crt_route = 0;
                    permute_paths(available_hops);
                }
                ecmp_choice = _crt_route % available_hops->size();
                _crt_route ++;
                break;
            case RR_ECMP:
                if (_type == TOR){
                    if (_crt_route>=5 * available_hops->size()){
                        _crt_route = 0;
                        permute_paths(available_hops);
                    }
                    ecmp_choice = _crt_route % available_hops->size();
                    _crt_route ++;
                }
                else ecmp_choice = freeBSDHash(pkt.flow_id(),pkt.pathid(),_hash_salt) % available_hops->size();
                
                break;
            }
        FibEntry* e = (*available_hops)[ecmp_choice];
        pkt.set_direction(e->getDirection());
        return e->getEgressPort();
    }

    // Adding to routing table
    //no route table entries for this destination. Add them to FIB or fail. 
    if (_type == TOR){
        // cerr << "TOR switch " << _id << " routing packet to dest " << pkt.dst() << endl;
        
        // Check if destination is directly connected using adjusted host ID
        uint32_t adjusted_dst = _ft->adjusted_host(pkt.dst());
        if ( _ft->HOST_POD_SWITCH(adjusted_dst) == _id) { 
            //this host is directly connected!
            // cerr << "  TOR switch " << _id << " dest is directly connected (adjusted host " << adjusted_dst << ")" << endl;
            HostFibEntry* fe = _fib->getHostRoute(pkt.dst(),pkt.flow_id());
            assert(fe);
            pkt.set_direction(DOWN);
            return fe->getEgressPort();
            
        } else {
            // cerr << "  TOR switch " << _id << " dest is not directly connected" << endl;
            
            // Check if this is inter-DC traffic
            if (is_inter_dc_traffic(pkt.dst())) {
                // cerr << "  TOR switch " << _id << " routing inter-DC traffic up" << endl;
                // Route inter-DC traffic up to AGG switches
                if (_uproutes)
                    _fib->setRoutes(pkt.dst(),_uproutes);
                else {
                    uint32_t podid,agg_min,agg_max;

                    if (_ft->get_tiers()==3) {
                        podid = _id / _ft->tor_switches_per_pod();
                        agg_min = _ft->MIN_POD_AGG_SWITCH(podid);
                        agg_max = _ft->MAX_POD_AGG_SWITCH(podid);
                    }
                    else {
                        agg_min = 0;
                        agg_max = _ft->getNAGG()-1;
                    }

                    for (uint32_t k=agg_min; k<=agg_max;k++){
                        for (uint32_t b = 0; b < _ft->bundlesize(AGG_TIER); b++) {
                            Route * r = new Route();
                            r->push_back(_ft->queues_nlp_nup[_id][k][b]);
                            assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                            r->push_back(_ft->pipes_nlp_nup[_id][k][b]);
                            r->push_back(_ft->queues_nlp_nup[_id][k][b]->getRemoteEndpoint());
                            _fib->addRoute(pkt.dst(),r,1,UP);
                        }
                    }
                    _uproutes = _fib->getRoutes(pkt.dst());
                    permute_paths(_uproutes);
                }
            } else {
                // cerr << "  TOR switch " << _id << " routing local traffic up (should not happen)" << endl;
                // cerr << "  Destination host " << pkt.dst() << " is in same DC but not directly connected" << endl;
                // cerr << "  TOR DC ID: " << _dc_id << ", dest DC: " << (pkt.dst() / _nodes_per_dc) << endl;
                return nullptr; // This will cause the packet to be dropped
            }
        }
    } else if (_type == AGG) {
        // cerr << "AGG switch " << _id << " routing packet to dest " << pkt.dst() << endl;
        
        // Check if destination is in the same pod (local traffic)
        uint32_t adjusted_dst = _ft->adjusted_host(pkt.dst());
        if ( _ft->get_tiers()==2 || _ft->HOST_POD(adjusted_dst) == _ft->AGG_SWITCH_POD_ID(_id)) {
            //must go down!
            // cerr << "  AGG switch " << _id << " routing down to local pod" << endl;
            //target NLP id is 2 * pkt.dst()/K
            uint32_t target_tor = _ft->HOST_POD_SWITCH(adjusted_dst);
            for (uint32_t b = 0; b < _ft->bundlesize(AGG_TIER); b++) {
                Route * r = new Route();
                r->push_back(_ft->queues_nup_nlp[_id][target_tor][b]);
                assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                r->push_back(_ft->pipes_nup_nlp[_id][target_tor][b]);          
                r->push_back(_ft->queues_nup_nlp[_id][target_tor][b]->getRemoteEndpoint());

                _fib->addRoute(pkt.dst(),r,1, DOWN);
            }
        } else {
            // cerr << "  AGG switch " << _id << " routing up to different pod" << endl;
            
            // Check if this is inter-DC traffic
            if (is_inter_dc_traffic(pkt.dst())) {
                // cerr << "  AGG switch " << _id << " routing inter-DC traffic up to CORE" << endl;
                // Route inter-DC traffic up to CORE switches
                if (_uproutes)
                    _fib->setRoutes(pkt.dst(),_uproutes);
                else {
                    uint32_t podpos = _id % _ft->agg_switches_per_pod();
                    uint32_t uplink_bundles = _ft->radix_up(AGG_TIER) / _ft->bundlesize(CORE_TIER);
                    for (uint32_t l = 0; l <  uplink_bundles ; l++) {
                        uint32_t core = l * _ft->agg_switches_per_pod() + podpos;
                        for (uint32_t b = 0; b < _ft->bundlesize(CORE_TIER); b++) {
                            Route *r = new Route();
                            r->push_back(_ft->queues_nup_nc[_id][core][b]);
                            assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                            r->push_back(_ft->pipes_nup_nc[_id][core][b]);
                            r->push_back(_ft->queues_nup_nc[_id][core][b]->getRemoteEndpoint());

                            _fib->addRoute(pkt.dst(),r,1,UP);
                        }
                    }
                    permute_paths(_fib->getRoutes(pkt.dst()));
                }
            } else {
                // cerr << "  AGG switch " << _id << " routing local traffic up (should not happen)" << endl;
                // cerr << "  Destination host " << pkt.dst() << " is in same DC but different pod" << endl;
                // cerr << "  AGG DC ID: " << _dc_id << ", dest DC: " << (pkt.dst() / _nodes_per_dc) << endl;
                return nullptr; // This will cause the packet to be dropped
            }
        }
    // Option for up route to send to other DC 
    // If staying in DC never go to the fat tree switch at the top
    } else if (_type == CORE) {
        // cerr << "CORE switch " << _id << " routing packet to dest " << pkt.dst() << endl;
        
        // Check if this is inter-DC traffic
        if (is_inter_dc_traffic(pkt.dst())) {
            // cerr << "  CORE switch " << _id << " routing inter-DC traffic to WAN switch" << endl;
            // cerr << "  Destination host " << pkt.dst() << " is in different DC" << endl;
            // cerr << "  CORE switch " << _id << " DC ID: " << _dc_id << ", dest DC: " << (pkt.dst() / _nodes_per_dc) << endl;
            // cerr << "  CORE switch " << _id << " connected to FatTree topology with host offset: " << (_ft ? _ft->_host_id_offset : -1) << endl;
            
            // Check if we have a WAN switch connected
            if (!_ft->get_wan_switch()) {
                cerr << "  ERROR: No WAN switch connected to FatTree topology" << endl;
                return nullptr;
            }
            
            // Create route to WAN switch through physical connections
            // We need to find the actual queue and pipe to the WAN switch
            Route* r = new Route();
            
            // Find the queue from this CORE switch to the WAN switch
            // The queue should be in the CORE switch's ports
            bool found_wan_connection = false;
            for (size_t i = 0; i < _ports.size(); i++) {
                BaseQueue* q = _ports[i];
                if (q && q->getRemoteEndpoint()) {
                    // Check if this queue leads to the WAN switch
                    Pipe* pipe = dynamic_cast<Pipe*>(q->getRemoteEndpoint());
                    if (pipe && pipe->getRemoteEndpoint() == _ft->get_wan_switch()) {
                        r->push_back(q);
                        r->push_back(pipe);
                        r->push_back(_ft->get_wan_switch());
                        found_wan_connection = true;
                        break;
                    }
                }
            }
            
            if (!found_wan_connection) {
                cerr << "  ERROR: CORE switch " << _id << " has no physical connection to WAN switch" << endl;
                delete r;
                return nullptr;
            }
            
            _fib->addRoute(pkt.dst(), r, 1, UP);
            
            // cerr << "  Created route to WAN switch for host " << pkt.dst() << endl;
        } else {
            // cerr << "  CORE switch " << _id << " routing local traffic down to AGG" << endl;
            // Route local traffic down to AGG switches
            uint32_t adjusted_dst = _ft->adjusted_host(pkt.dst());
            uint32_t nup = _ft->MIN_POD_AGG_SWITCH(_ft->HOST_POD(adjusted_dst)) + (_id % _ft->agg_switches_per_pod());
            for (uint32_t b = 0; b < _ft->bundlesize(CORE_TIER); b++) {
                Route *r = new Route();
                // cout << "CORE switch " << _id << " adding route to " << pkt.dst() << " via AGG " << nup << endl;

                assert (_ft->queues_nc_nup[_id][nup][b]);
                r->push_back(_ft->queues_nc_nup[_id][nup][b]);
                assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                assert (_ft->pipes_nc_nup[_id][nup][b]);
                r->push_back(_ft->pipes_nc_nup[_id][nup][b]);

                r->push_back(_ft->queues_nc_nup[_id][nup][b]->getRemoteEndpoint());
                _fib->addRoute(pkt.dst(),r,1,DOWN);
            }
        }
    }
    else {
        cerr << "Route lookup on switch with no proper type: " << _type << endl;
        abort();
    }
    assert(_fib->getRoutes(pkt.dst()));

    //FIB has been filled in; return choice. 
    return getNextHop(pkt, ingress_port);
};

// Helper to check if traffic is inter-DC
bool MultiFatTreeSwitch::is_inter_dc_traffic(uint32_t dest_host) const {
    // If we don't have multi-DC info, assume it's local traffic
    if (_nodes_per_dc == 0) {
        return false;
    }
    
    uint32_t dest_dc = dest_host / _nodes_per_dc;
    bool is_inter = (dest_dc != _dc_id);
    
    // Debug output for CORE switches (disabled for performance)
    // if (_type == CORE) {
    //     std::cout << "  CORE switch " << _id << " checking if host " << dest_host << " is inter-DC" << std::endl;
    //     std::cout << "    dest_dc=" << dest_dc << ", _dc_id=" << _dc_id << ", is_inter=" << is_inter << std::endl;
    // }
    
    return is_inter;
}

// Public method to add routes to FIB (for WAN switches)
void MultiFatTreeSwitch::add_wan_route(uint32_t dest_host, Route* route) {
    if (_type != WAN) {
        cerr << "Warning: Attempting to add WAN route to non-WAN switch" << endl;
        return;
    }
    
    _fib->addRoute(dest_host, route, 1, DOWN);
}
