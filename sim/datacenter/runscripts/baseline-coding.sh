#!/bin/bash

# # ECMP
# for i in {1..10}
# do
#     ./htsim_consterase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp -of ../../results/opt-4-2/baseline-coding/ecmp/opt-coding-ata-128n-2MB-ecmp-${i}.csv -ratecoef 0.9 -k 0
# done

# # # PLB
# for i in {1..10}
# do
#     ./htsim_consterase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp -hostlb plb -plbecn 4 -of ../../results/opt-4-2/baseline-coding/plb/opt-coding-ata-128n-2MB-plb4_06-${i}.csv -ratecoef 0.9 -k 0
# done

# # RR
# for i in {1..10}
# do
#     ./htsim_consterase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat rr -of ../../results/opt-4-2/baseline-coding/rr/opt-coding-ata-128n-2MB-rr-${i}.csv -ratecoef 0.9 -k 0
# done

# # Host spray
# for i in {1..10}
# do
#     ./htsim_consterase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp -hostlb spray -of ../../results/opt-4-2/baseline-coding/spray/opt-coding-ata-128n-2MB-spray-${i}.csv -ratecoef 0.9 -k 0
# done

# # ECMP AR
# for i in {1..10}
# do
#     ./htsim_consterase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp_ar -of ../../results/opt-4-2/baseline-coding/ecmp-ar/opt-coding-ata-128n-2MB-ecmpar-${i}.csv -ratecoef 0.9 -k 0
# done


# # Flowlet AR
# for i in {1..10}
# do
#     ./htsim_consterase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat fl_ar -of ../../results/opt-4-2/baseline-coding/flowlet-ar/opt-coding-ata-128n-2MB-flowletar-${i}.csv -ratecoef 0.9 -k 0
# done

# # Packet AR
# for i in {1..10}
# do
#     ./htsim_consterase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat pkt_ar -of ../../results/opt-4-2/baseline-coding/packet-ar/opt-coding-ata-128n-2MB-packetar-${i}.csv -ratecoef 0.9 -k 0
# done

# ECMP
for i in {1..1}
do
    ./htsim_consterase -end 0 -tm ./connection_matrices/alltoall_128n_2MB.cm -nodes 128 -strat ecmp -of ../../results/opt-4-2/baseline-coding/subflows2-ecmp/opt-ata-128n-2MB-sf2-ecmp-${i}.csv -ratecoef 0.9 -subflows 2
done

