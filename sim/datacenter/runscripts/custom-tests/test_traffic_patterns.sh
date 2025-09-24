#!/bin/bash

# Traffic Pattern Tests
# Tests different traffic patterns and connection matrices

echo "=== Traffic Pattern Test Suite ==="
echo "Starting at $(date)"

# Create results directory
mkdir -p ../../../results/test-suite/traffic-patterns

# Test parameters
RUNS=3
STRATEGY="ecmp"
HOST_LB="spray"

# Different traffic patterns to test
TRAFFIC_PATTERNS=(
    "alltoall_128n_2MB.cm:All-to-All"
    "perm_128n_128c_2MB.cm:Permutation"
    "incast.cm:Incast"
    "outcast_incast.cm:Outcast-Incast"
    "rand_128n_1280c_2MB.cm:Random"
)

# Test each traffic pattern
for pattern_info in "${TRAFFIC_PATTERNS[@]}"
do
    IFS=':' read -r matrix_file pattern_name <<< "$pattern_info"
    
    echo "Testing traffic pattern: $pattern_name ($matrix_file)"
    
    # Determine appropriate node count based on matrix
    if [[ $matrix_file == *"128n"* ]]; then
        nodes=128
    elif [[ $matrix_file == *"16n"* ]]; then
        nodes=16
    elif [[ $matrix_file == *"32n"* ]]; then
        nodes=32
    else
        nodes=128  # Default
    fi
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../connection_matrices/$matrix_file \
            -nodes $nodes \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/traffic-patterns/pattern-${pattern_name//-/_}-run${run}.csv
    done
done

# Test different flow sizes
echo "Testing different flow sizes"
FLOW_SIZES=(1048576 2097152 4194304 8388608)  # 1MB, 2MB, 4MB, 8MB

for flow_size in "${FLOW_SIZES[@]}"
do
    echo "Testing flow size: $flow_size bytes"
    
    # Generate custom connection matrix with specific flow size
    python3 ../connection_matrices/gen_multidc_alltoall.py \
        ../../../results/test-suite/traffic-patterns/custom_${flow_size}b.cm \
        128 2 $flow_size 0 1
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../../../results/test-suite/traffic-patterns/custom_${flow_size}b.cm \
            -nodes 128 \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/traffic-patterns/flowsize-${flow_size}b-run${run}.csv
    done
done

# Test different connection densities
echo "Testing different connection densities"
CONNECTION_COUNTS=(128 256 512 1024 2048)

for conn_count in "${CONNECTION_COUNTS[@]}"
do
    echo "Testing connection count: $conn_count"
    
    # Generate permutation matrix with specific connection count
    python3 ../connection_matrices/gen_permutation.py \
        ../../../results/test-suite/traffic-patterns/perm_128n_${conn_count}c_2MB.cm \
        128 $conn_count 2097152 0 1
    
    for run in $(seq 1 $RUNS)
    do
        echo "  Run $run/$RUNS at $(date)"
        
        ../htsim_multi_dc \
            -end 0 \
            -tm ../../../results/test-suite/traffic-patterns/perm_128n_${conn_count}c_2MB.cm \
            -nodes 128 \
            -strat $STRATEGY \
            -hostlb $HOST_LB \
            -ratecoef 0.9 \
            -subflows 2 \
            -of ../../../results/test-suite/traffic-patterns/conns-${conn_count}-run${run}.csv
    done
done

echo "Traffic pattern tests completed at $(date)"
