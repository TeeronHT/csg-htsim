#!/bin/bash

# Master Test Suite Runner
# Runs all comprehensive tests for the multi-datacenter simulation framework

echo "=========================================="
echo "Multi-Datacenter Simulation Test Suite"
echo "=========================================="
echo "Starting comprehensive testing at $(date)"
echo ""

# Create main results directory
mkdir -p ../../../results/test-suite

# Make all test scripts executable
chmod +x test_*.sh

# Test 1: Routing Strategies
echo "=== TEST 1: Routing Strategies ==="
echo "Testing all routing strategies with different subflow configurations..."
bash test_routing_strategies.sh
echo "Routing strategy tests completed at $(date)"
echo ""

# Test 2: Host Load Balancing
echo "=== TEST 2: Host Load Balancing ==="
echo "Testing host load balancing strategies..."
bash test_host_lb.sh
echo "Host load balancing tests completed at $(date)"
echo ""

# Test 3: Topology and Scale
echo "=== TEST 3: Topology and Scale ==="
echo "Testing different datacenter configurations and scales..."
bash test_topology_scale.sh
echo "Topology and scale tests completed at $(date)"
echo ""

# Test 4: Traffic Patterns
echo "=== TEST 4: Traffic Patterns ==="
echo "Testing different traffic patterns and connection matrices..."
bash test_traffic_patterns.sh
echo "Traffic pattern tests completed at $(date)"
echo ""

# Test 5: Reliability and Failures
echo "=== TEST 5: Reliability and Failures ==="
echo "Testing system behavior under failure scenarios..."
bash test_reliability_failures.sh
echo "Reliability and failure tests completed at $(date)"
echo ""

# Generate test summary
echo "=== TEST SUMMARY ==="
echo "All tests completed at $(date)"
echo ""
echo "Results directory structure:"
find ../../../results/test-suite -type f -name "*.csv" | wc -l | xargs echo "Total result files:"
echo ""
echo "Test categories:"
echo "  - Routing strategies: $(find ../../../results/test-suite/routing-strategies -name "*.csv" 2>/dev/null | wc -l) files"
echo "  - Host load balancing: $(find ../../../results/test-suite/host-load-balancing -name "*.csv" 2>/dev/null | wc -l) files"
echo "  - Topology and scale: $(find ../../../results/test-suite/topology-scale -name "*.csv" 2>/dev/null | wc -l) files"
echo "  - Traffic patterns: $(find ../../../results/test-suite/traffic-patterns -name "*.csv" 2>/dev/null | wc -l) files"
echo "  - Reliability and failures: $(find ../../../results/test-suite/reliability-failures -name "*.csv" 2>/dev/null | wc -l) files"
echo ""
echo "=========================================="
echo "Comprehensive test suite completed!"
echo "=========================================="
