#!/bin/bash

# Topology and Scale Tests
# Tests different datacenter configurations and scales

echo "=== Topology and Scale Test Suite ==="
echo "Starting at $(date)"

# Create results directory
mkdir -p ../../../results/test-suite/topology-scale

# Test parameters
RUNS=3  # Reduced for scale testing
STRATEGY="ecmp"
HOST_LB="spray"

# Different datacenter configurations
DC_CONFIGS=("2" "4" "8")
NODES_PER_DC=(64 32 16)

# Test different datacenter scales
for i in "${!DC_CONFIGS[@]}"
do
    dcs=${DC_CONFIGS[$i]}
    nodes_per_dc=${NODES_PER_DC[$i]}
    total_nodes=$((dcs * nodes_per_dc))
    
    echo "Testing $dcs datacenters with $nodes_per_dc nodes each (total: $total_nodes)"
    
    # Generate connection matrix for this configuration
    python3 ../connection_matrices/gen_multidc_alltoall.py \
        ../../../results/test-suite/topology-scale/multidc_${total_nodes}n_${dcs}dc_2MB.cm \
        $total_nodes $dcs 2097152 0 1
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../../../results/test-suite/topology-scale/multidc_${total_nodes}n_${dcs}dc_2MB.cm \
            -nodes $total_nodes \
            -dcs $dcs \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/topology-scale/topology-${dcs}dc-${total_nodes}n-run${run}.csv
    done
done

# Test different WAN configurations
echo "Testing different WAN configurations"
WAN_SPEEDS=(10 25 50 100)  # Gbps
WAN_DELAYS=(0.1 0.5 1.0 2.0)  # ms

for i in "${!WAN_SPEEDS[@]}"
do
    wan_speed=${WAN_SPEEDS[$i]}
    wan_delay=${WAN_DELAYS[$i]}
    
    echo "Testing WAN: ${wan_speed}Gbps, ${wan_delay}ms delay"
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
            -nodes 256 \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -wan_speed $wan_speed \
            -wan_delay $wan_delay \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/topology-scale/wan-${wan_speed}gbps-${wan_delay}ms-run${run}.csv
    done
done

echo "Topology and scale tests completed at $(date)"
