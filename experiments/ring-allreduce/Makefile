# Build the ring-AllReduce driver against the existing simulator.
# Topology sources stay in sim/datacenter; this directory only compiles
# the experiment program and links those objects in.

SIM := ../../sim
DC := $(SIM)/datacenter

CXX := g++
CXXFLAGS := -Wall -std=c++11 -g -Wsign-compare -O2
INCLUDES := -I$(SIM) -I$(DC)

SRCS := main_ring.cpp ring_runner.cpp ring_planner.cpp ring_schedule.cpp ring_metrics.cpp
OBJS := $(SRCS:.cpp=.o)

TOPO_OBJS := $(DC)/firstfit.o $(DC)/fat_tree_topology.o $(DC)/fat_tree_switch.o $(DC)/connection_matrix.o

.PHONY: all clean

all: htsim_ring

htsim_ring: $(OBJS) $(SIM)/libhtsim.a $(TOPO_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(TOPO_OBJS) -L$(SIM) -lhtsim

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(SIM)/libhtsim.a:
	$(MAKE) -C $(SIM) libhtsim.a

$(TOPO_OBJS):
	$(MAKE) -C $(DC) firstfit.o fat_tree_topology.o fat_tree_switch.o connection_matrix.o

clean:
	rm -f $(OBJS) htsim_ring
