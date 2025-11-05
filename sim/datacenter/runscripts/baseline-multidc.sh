#!/bin/bash

# Create output directories
echo "Creating output directories..."
mkdir -p ../../results/opt-4-2/baseline-multidc/subflows2-ecmp
mkdir -p ../../results/opt-4-2/baseline-multidc/ecmp
mkdir -p ../../results/opt-4-2/baseline-multidc/plb
mkdir -p ../../results/opt-4-2/baseline-multidc/rr
mkdir -p ../../results/opt-4-2/baseline-multidc/spray
mkdir -p ../../results/opt-4-2/baseline-multidc/ecmp-ar
mkdir -p ../../results/opt-4-2/baseline-multidc/flowlet-ar
mkdir -p ../../results/opt-4-2/baseline-multidc/packet-ar

echo "Starting simulations..."

# ECMP (2 subflows)
echo "Running ECMP with 2 subflows..."
for i in {1..3}
do
    echo "  Run ${i}/3..."
    ./htsim_multi_dc -end 0 -tm ./connection_matrices/multidc_256n_2dc_2MB.cm -nodes 256 -strat ecmp -of ../../results/opt-4-2/baseline-multidc/subflows2-ecmp/opt-multidc-256n-2MB-sf2-ecmp-${i}.csv -ratecoef 1.0 -subflows 2
done

# Multi-DC ECMP
echo "Running Multi-DC ECMP..."
for i in {1..3}
do
    echo "  Run ${i}/3..."
    ./htsim_multi_dc -end 0 -tm ./connection_matrices/multidc_256n_2dc_2MB.cm -nodes 256 -strat ecmp -of ../../results/opt-4-2/baseline-multidc/ecmp/opt-multidc-256n-2MB-ecmp-${i}.csv -ratecoef 1.0 -k 0
done

# Multi-DC PLB
echo "Running Multi-DC PLB..."
for i in {1..3}
do
    echo "  Run ${i}/3..."
    ./htsim_multi_dc -end 0 -tm ./connection_matrices/multidc_256n_2dc_2MB.cm -nodes 256 -strat ecmp -hostlb plb -plbecn 4 -of ../../results/opt-4-2/baseline-multidc/plb/opt-multidc-256n-2MB-plb-${i}.csv -ratecoef 1.0 -k 0
done

# Multi-DC RR
echo "Running Multi-DC Round Robin..."
for i in {1..3}
do
    echo "  Run ${i}/3..."
    ./htsim_multi_dc -end 0 -tm ./connection_matrices/multidc_256n_2dc_2MB.cm -nodes 256 -strat rr -of ../../results/opt-4-2/baseline-multidc/rr/opt-multidc-256n-2MB-rr-${i}.csv -ratecoef 1.0 -k 0
done

# Multi-DC Host spray
echo "Running Multi-DC Host Spray..."
for i in {1..3}
do
    echo "  Run ${i}/3..."
    ./htsim_multi_dc -end 0 -tm ./connection_matrices/multidc_256n_2dc_2MB.cm -nodes 256 -strat ecmp -hostlb spray -of ../../results/opt-4-2/baseline-multidc/spray/opt-multidc-256n-2MB-spray-${i}.csv -ratecoef 1.0 -k 0
done

# Multi-DC ECMP AR
echo "Running Multi-DC ECMP AR..."
for i in {1..3}
do
    echo "  Run ${i}/3..."
    ./htsim_multi_dc -end 0 -tm ./connection_matrices/multidc_256n_2dc_2MB.cm -nodes 256 -strat ecmp_ar -of ../../results/opt-4-2/baseline-multidc/ecmp-ar/opt-multidc-256n-2MB-ecmp-ar-${i}.csv -ratecoef 1.0 -k 0
done

# Multi-DC Flowlet AR
echo "Running Multi-DC Flowlet AR..."
for i in {1..3}
do
    echo "  Run ${i}/3..."
    ./htsim_multi_dc -end 0 -tm ./connection_matrices/multidc_256n_2dc_2MB.cm -nodes 256 -strat fl_ar -of ../../results/opt-4-2/baseline-multidc/flowlet-ar/opt-multidc-256n-2MB-flowlet-ar-${i}.csv -ratecoef 1.0 -k 0
done

# Multi-DC Packet AR
echo "Running Multi-DC Packet AR..."
for i in {1..3}
do
    echo "  Run ${i}/3..."
    ./htsim_multi_dc -end 0 -tm ./connection_matrices/multidc_256n_2dc_2MB.cm -nodes 256 -strat pkt_ar -of ../../results/opt-4-2/baseline-multidc/packet-ar/opt-multidc-256n-2MB-packet-ar-${i}.csv -ratecoef 1.0 -k 0
done

echo "All simulations completed!" 