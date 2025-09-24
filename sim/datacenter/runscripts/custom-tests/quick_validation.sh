#!/bin/bash

# Quick Validation Tests
# Fast tests to verify the framework is working correctly

echo "=== Quick Validation Tests ==="
echo "Starting quick validation at $(date)"

# Create results directory
mkdir -p ../../../results/validation

# Test 1: Basic functionality
echo "Test 1: Basic multi-DC simulation"
../htsim_multi_dc \
    -end 0 \
    -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
    -nodes 128 \
    -strat ecmp \
    -ratecoef 0.9 \
    -subflows 2 \
    -of ../../../results/validation/basic-test.csv

if [ $? -eq 0 ]; then
    echo "✅ Basic test passed"
else
    echo "❌ Basic test failed"
    exit 1
fi

# Test 2: Different routing strategy
echo "Test 2: Different routing strategy (RR)"
../htsim_multi_dc \
    -end 0 \
    -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
    -nodes 128 \
    -strat rr \
    -ratecoef 0.9 \
    -subflows 2 \
    -of ../../../results/validation/rr-test.csv

if [ $? -eq 0 ]; then
    echo "✅ RR routing test passed"
else
    echo "❌ RR routing test failed"
    exit 1
fi

# Test 3: Host load balancing
echo "Test 3: Host load balancing (spray)"
../htsim_multi_dc \
    -end 0 \
    -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
    -nodes 128 \
    -strat ecmp \
    -hostlb spray \
    -ratecoef 0.9 \
    -subflows 2 \
    -of ../../../results/validation/spray-test.csv

if [ $? -eq 0 ]; then
    echo "✅ Spray test passed"
else
    echo "❌ Spray test failed"
    exit 1
fi

# Test 4: Different topology
echo "Test 4: Different topology (4 datacenters)"
../htsim_multi_dc \
    -end 0 \
    -tm ../connection_matrices/multidc_256n_2dc_2MB.cm \
    -nodes 128 \
    -dcs 4 \
    -strat ecmp \
    -ratecoef 0.9 \
    -subflows 2 \
    -of ../../../results/validation/4dc-test.csv

if [ $? -eq 0 ]; then
    echo "✅ 4-DC topology test passed"
else
    echo "❌ 4-DC topology test failed"
    exit 1
fi

# Test 5: Different traffic pattern
echo "Test 5: Different traffic pattern (all-to-all)"
../htsim_multi_dc \
    -end 0 \
    -tm ../connection_matrices/alltoall_128n_2MB.cm \
    -nodes 128 \
    -strat ecmp \
    -ratecoef 0.9 \
    -subflows 2 \
    -of ../../../results/validation/alltoall-test.csv

if [ $? -eq 0 ]; then
    echo "✅ All-to-all traffic test passed"
else
    echo "❌ All-to-all traffic test failed"
    exit 1
fi

# Verify results
echo ""
echo "=== Validation Results ==="
echo "Checking result files..."
for file in ../../../results/validation/*.csv; do
    if [ -f "$file" ]; then
        lines=$(wc -l < "$file")
        echo "✅ $file: $lines lines"
    else
        echo "❌ $file: Missing"
    fi
done

echo ""
echo "Quick validation completed at $(date)"
echo "All basic tests passed! Framework is ready for comprehensive testing."
