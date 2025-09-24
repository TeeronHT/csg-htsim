# Custom Test Suite for Multi-Datacenter Simulation

This directory contains comprehensive test scripts for validating the multi-datacenter network simulation framework.

## Test Scripts Overview

### Quick Validation Tests
- **`quick_validation.sh`** - Fast validation tests (5-10 minutes)
  - Tests basic functionality, routing strategies, host LB, topology, traffic patterns
  - Use this first to verify the framework is working

### Individual Test Suites
- **`test_routing_strategies.sh`** - Tests all routing strategies
  - ECMP, RR, ECMP_AR, PKT_AR, FL_AR, RR_ECMP
  - Different subflow counts (1, 2, 4, 8)
  - Runtime: ~30-60 minutes

- **`test_host_lb.sh`** - Tests host load balancing strategies
  - Spray, PLB, Spray-Adaptive
  - Different ECN thresholds for PLB
  - Runtime: ~20-40 minutes

- **`test_topology_scale.sh`** - Tests topology and scale variations
  - Different DC counts (2, 4, 8)
  - Different WAN speeds and delays
  - Runtime: ~30-60 minutes

- **`test_traffic_patterns.sh`** - Tests different traffic patterns
  - All-to-all, permutation, incast, random patterns
  - Different flow sizes and connection densities
  - Runtime: ~40-80 minutes

- **`test_reliability_failures.sh`** - Tests reliability and failure scenarios
  - Link failures, flaky links, congestion levels
  - Different failure counts and queue sizes
  - Runtime: ~30-60 minutes

### Master Test Runner
- **`run_all_tests.sh`** - Runs all test suites in sequence
  - Complete validation of the framework
  - Runtime: ~3-5 hours total

## Usage

### From the runscripts directory:
```bash
# Quick validation
bash custom-tests/quick_validation.sh

# Individual test suites
bash custom-tests/test_routing_strategies.sh
bash custom-tests/test_host_lb.sh
bash custom-tests/test_topology_scale.sh
bash custom-tests/test_traffic_patterns.sh
bash custom-tests/test_reliability_failures.sh

# Complete test suite
bash custom-tests/run_all_tests.sh
```

### From the custom-tests directory:
```bash
cd custom-tests

# Quick validation
bash quick_validation.sh

# Individual test suites
bash test_routing_strategies.sh
bash test_host_lb.sh
bash test_topology_scale.sh
bash test_traffic_patterns.sh
bash test_reliability_failures.sh

# Complete test suite
bash run_all_tests.sh
```

## Results

Test results are saved in:
- `../../results/validation/` - Quick validation results
- `../../results/test-suite/` - Comprehensive test results
  - `routing-strategies/` - Routing strategy test results
  - `host-load-balancing/` - Host LB test results
  - `topology-scale/` - Topology and scale test results
  - `traffic-patterns/` - Traffic pattern test results
  - `reliability-failures/` - Reliability and failure test results

## Prerequisites

- Multi-datacenter simulation built (`htsim_multi_dc`)
- Connection matrix files in `../connection_matrices/`
- Python3 for generating custom connection matrices

## Expected Output

Each test generates CSV files with simulation results including:
- Flow completion times
- Throughput measurements
- Packet loss statistics
- Inter-DC vs Intra-DC flow performance
- Various performance metrics

## Troubleshooting

- Ensure all scripts are executable: `chmod +x *.sh`
- Check that `htsim_multi_dc` binary exists and is executable
- Verify connection matrix files are present
- Monitor disk space for large result files
