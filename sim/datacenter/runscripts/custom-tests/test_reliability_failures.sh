#!/bin/bash

# Reliability and Failure Tests
# Tests system behavior under various failure scenarios

echo "=== Reliability and Failure Test Suite ==="
echo "Starting at $(date)"

# Create results directory
mkdir -p ../../../results/test-suite/reliability-failures

# Test parameters
RUNS=3
STRATEGY="ecmp"
HOST_LB="spray"

# Test different failure scenarios
FAILURE_COUNTS=(0 1 2 4 8)
FAILURE_PERCENTAGES=(0.0 0.1 0.2 0.5)

echo "Testing link failures"
for fail_count in "${FAILURE_COUNTS[@]}"
do
    echo "Testing with $fail_count link failures"
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
            -nodes 256 \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -fails $fail_count \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/reliability-failures/failures-${fail_count}-run${run}.csv
    done
done

echo "Testing link failure percentages"
for fail_pct in "${FAILURE_PERCENTAGES[@]}"
do
    echo "Testing with $fail_pct failure percentage"
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
            -nodes 256 \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -fails 4 \
            -failpct $fail_pct \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/reliability-failures/failpct-${fail_pct}-run${run}.csv
    done
done

# Test flaky links
echo "Testing flaky links"
FLAKY_LINK_COUNTS=(0 2 4 8)

for flaky_count in "${FLAKY_LINK_COUNTS[@]}"
do
    echo "Testing with $flaky_count flaky links"
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
            -nodes 256 \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -flakylinks $flaky_count \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/reliability-failures/flaky-${flaky_count}-run${run}.csv
    done
done

# Test different rate coefficients (congestion levels)
echo "Testing different congestion levels"
RATE_COEFFICIENTS=(0.5 0.7 0.9 1.1 1.3)

for rate_coef in "${RATE_COEFFICIENTS[@]}"
do
    echo "Testing rate coefficient: $rate_coef"
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
            -nodes 256 \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -ratecoef $rate_coef \
            -subflows 2 \
            -of ../../../results/test-suite/reliability-failures/ratecoef-${rate_coef}-run${run}.csv
    done
done

# Test with different queue sizes
echo "Testing different queue sizes"
QUEUE_SIZES=(4 8 16 32 64)

for queue_size in "${QUEUE_SIZES[@]}"
do
    echo "Testing queue size: $queue_size"
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
            -nodes 256 \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -q $queue_size \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/reliability-failures/queue-${queue_size}-run${run}.csv
    done
done

echo "Reliability and failure tests completed at $(date)"
