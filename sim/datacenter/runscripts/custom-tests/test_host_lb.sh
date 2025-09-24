#!/bin/bash

# Host Load Balancing Strategy Tests
# Tests different host load balancing approaches

echo "=== Host Load Balancing Test Suite ==="
echo "Starting at $(date)"

# Create results directory
mkdir -p ../../../results/test-suite/host-load-balancing

# Test parameters
NODES=128
STRATEGY="ecmp"
RUNS=5

# Host load balancing strategies
HOST_LB_STRATEGIES=("spray" "plb" "sprayad")

# Test each host load balancing strategy
for host_lb in "${HOST_LB_STRATEGIES[@]}"
do
    echo "Testing host load balancing: $host_lb"
    
    # Test with different ECN thresholds for PLB
    if [ "$host_lb" = "plb" ]; then
        ECN_THRESHOLDS=(2 4 8 16)
        for ecn_thresh in "${ECN_THRESHOLDS[@]}"
        do
            echo "  Testing PLB with ECN threshold: $ecn_thresh"
            
            for i in $(seq 1 $RUNS)
            do
                echo "    Run $i/$RUNS at $(date)"
                
                ../htsim_multi_dc \
                    -end 0 \
                    -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
                    -nodes $NODES \
                    -strat $STRATEGY \
                    -hostlb $host_lb \
                    -plbecn $ecn_thresh \
                    -ratecoef 0.9 \
                    -subflows 2 \
                    -of ../../../results/test-suite/host-load-balancing/hostlb-${host_lb}-ecn${ecn_thresh}-run${i}.csv
            done
        done
    else
        # Test spray and sprayad strategies
        for i in $(seq 1 $RUNS)
        do
            echo "  Run $i/$RUNS at $(date)"
            
            ../htsim_multi_dc \
                -end 0 \
                -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
                -nodes $NODES \
                -strat $STRATEGY \
                -hostlb $host_lb \
                -ratecoef 0.9 \
                -subflows 2 \
                -of ../../../results/test-suite/host-load-balancing/hostlb-${host_lb}-run${i}.csv
        done
    fi
done

echo "Host load balancing tests completed at $(date)"
