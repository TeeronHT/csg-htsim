#!/bin/bash

# Comprehensive Routing Strategy Tests for Multi-DC Simulation
# Tests all available routing strategies with different configurations

echo "=== Routing Strategy Test Suite ==="
echo "Starting at $(date)"

# Create results directory
mkdir -p ../../../results/test-suite/routing-strategies

# Test parameters
NODES=128
SUBSETS=(1 2 4 8)
RATE_COEF=0.9
RUNS=5  # Reduced for testing

# Available routing strategies
STRATEGIES=("ecmp" "rr" "ecmp_ar" "pkt_ar" "fl_ar" "rr_ecmp")

# Test each routing strategy
for strategy in "${STRATEGIES[@]}"
do
    echo "Testing routing strategy: $strategy"
    
    # Test with different subflow counts
    for subflows in "${SUBSETS[@]}"
    do
        echo "  Testing with $subflows subflows"
        
        for i in $(seq 1 $RUNS)
        do
            echo "    Run $i/$RUNS at $(date)"
            
            ../htsim_multi_dc \
                -end 0 \
                -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
                -nodes $NODES \
                -strat $strategy \
                -subflows $subflows \
                -ratecoef $RATE_COEF \
                -of ../../../results/test-suite/routing-strategies/strategy-${strategy}-sf${subflows}-run${i}.csv
        done
    done
done

echo "Routing strategy tests completed at $(date)"
