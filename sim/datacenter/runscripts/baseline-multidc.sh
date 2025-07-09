#!/bin/bash

# Multi-DC ECMP
for i in {1..10}
do
    ./main_multi_dc_const_erase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp -of ../../results/opt-4-2/baseline-multidc/ecmp/opt-multidc-ata-128n-2MB-ecmp-${i}.csv -ratecoef 0.9 -k 0
done

# Multi-DC PLB
for i in {1..10}
do
    ./main_multi_dc_const_erase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp -hostlb plb -plbecn 4 -of ../../results/opt-4-2/baseline-multidc/plb/opt-multidc-ata-128n-2MB-plb4_06-${i}.csv -ratecoef 0.9 -k 0
done

# Multi-DC RR
for i in {1..10}
do
    ./main_multi_dc_const_erase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat rr -of ../../results/opt-4-2/baseline-multidc/rr/opt-multidc-ata-128n-2MB-rr-${i}.csv -ratecoef 0.9 -k 0
done

# Multi-DC Host spray
for i in {1..10}
do
    ./main_multi_dc_const_erase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp -hostlb spray -of ../../results/opt-4-2/baseline-multidc/spray/opt-multidc-ata-128n-2MB-spray-${i}.csv -ratecoef 0.9 -k 0
done

# Multi-DC ECMP AR
for i in {1..10}
do
    ./main_multi_dc_const_erase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp_ar -of ../../results/opt-4-2/baseline-multidc/ecmp-ar/opt-multidc-ata-128n-2MB-ecmpar-${i}.csv -ratecoef 0.9 -k 0
done

# Multi-DC Flowlet AR
for i in {1..10}
do
    ./main_multi_dc_const_erase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat fl_ar -of ../../results/opt-4-2/baseline-multidc/flowlet-ar/opt-multidc-ata-128n-2MB-flowletar-${i}.csv -ratecoef 0.9 -k 0
done

# Multi-DC Packet AR
for i in {1..10}
do
    ./main_multi_dc_const_erase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat pkt_ar -of ../../results/opt-4-2/baseline-multidc/packet-ar/opt-multidc-ata-128n-2MB-packetar-${i}.csv -ratecoef 0.9 -k 0
done 