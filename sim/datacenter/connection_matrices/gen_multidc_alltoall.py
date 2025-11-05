#!/usr/bin/env python3
import sys
from random import seed, shuffle

if len(sys.argv) != 8:
    print("Usage: python gen_multidc_alltoall.py <filename> <nodes> <dcs> <conns_per_dc_group> <groupsize> <flowsize> <randseed>")
    print("Note: For true all-to-all, groupsize is ignored for intra-DC connections.")
    sys.exit(1)

filename = sys.argv[1]
nodes = int(sys.argv[2])
dcs = int(sys.argv[3])
conns_per_dc = int(sys.argv[4])
groupsize = int(sys.argv[5])
flowsize = int(sys.argv[6])
randseed = int(sys.argv[7])

if nodes % dcs != 0:
    print("Number of nodes must be divisible by number of datacenters.")
    sys.exit(1)

nodes_per_dc = nodes // dcs

def intra_dc_alltoall(dc_offset, f, id_start):
    """Create true all-to-all connections within a datacenter.
    Each node connects to all other nodes in the same DC."""
    id = id_start
    for src in range(dc_offset, dc_offset + nodes_per_dc):
        for dst in range(dc_offset, dc_offset + nodes_per_dc):
            if src != dst:  # Exclude self-connections
                id += 1
                out = f"{src}->{dst} id {id} start 0 size {flowsize}"
                print(out, file=f)
    return id

def inter_dc_alltoall(dc_a, dc_b, f, id_start):
    id = id_start
    for src in range(dc_a*nodes_per_dc, (dc_a+1)*nodes_per_dc):
        for dst in range(dc_b*nodes_per_dc, (dc_b+1)*nodes_per_dc):
            if src != dst:
                id += 1
                out = f"{src}->{dst} id {id} start 0 size {flowsize}"
                print(out, file=f)
    return id

if randseed != 0:
    seed(randseed)

# First pass: count total connections
id = 0
# Intra-DC all-to-all: each node connects to all other nodes in same DC
for dc in range(dcs):
    for src in range(dc*nodes_per_dc, (dc+1)*nodes_per_dc):
        for dst in range(dc*nodes_per_dc, (dc+1)*nodes_per_dc):
            if src != dst:
                id += 1
# Inter-DC all-to-all (between every pair of DCs)
for dc_a in range(dcs):
    for dc_b in range(dcs):
        if dc_a != dc_b:
            for src in range(dc_a*nodes_per_dc, (dc_a+1)*nodes_per_dc):
                for dst in range(dc_b*nodes_per_dc, (dc_b+1)*nodes_per_dc):
                    if src != dst:
                        id += 1

with open(filename, "w") as f:
    print(f"Nodes {nodes}", file=f)
    print(f"Connections {id}", file=f)
    # Reset id for actual connection generation
    id = 0
    # Intra-DC all-to-all
    for dc in range(dcs):
        id = intra_dc_alltoall(dc*nodes_per_dc, f, id)
    # Inter-DC all-to-all (between every pair of DCs)
    for dc_a in range(dcs):
        for dc_b in range(dcs):
            if dc_a != dc_b:
                id = inter_dc_alltoall(dc_a, dc_b, f, id)
