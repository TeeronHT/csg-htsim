CC = g++
CFLAGS = -Wall -std=c++11 -g -Wsign-compare
#CFLAGS += -fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined
CFLAGS += -O2  
CRT:= $(shell pwd)
INCLUDE= -I"$(CRT)/.." -I"$(CRT)" 
LIB=-L..
DEPS=../libhtsim.a

all:	htsim_tcp htsim_ndp htsim_roce htsim_swift htsim_hpcc htsim_eqds htsim_constcca htsim_consterase htsim_constcca_old htsim_roce_new htsim_multi_dc


htsim_tcp: main_tcp.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o fat_tree_switch.o dragon_fly_topology.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o
	$(CC) $(CFLAGS) main_tcp.o firstfit.o vl2_topology.o dragon_fly_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o $(LIB) -lhtsim -o htsim_tcp


htsim_ndp: main_ndp.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o fat_tree_switch.o
	$(CC) $(CFLAGS) firstfit.o main_ndp.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o $(LIB) -lhtsim -o htsim_ndp

htsim_eqds: main_eqds.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o fat_tree_switch.o
	$(CC) $(CFLAGS) firstfit.o main_eqds.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o $(LIB) -lhtsim -o htsim_eqds


htsim_roce: main_roce.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o fat_tree_switch.o
	$(CC) $(CFLAGS) firstfit.o main_roce.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o $(LIB) -lhtsim -o htsim_roce

htsim_roce_new: main_roce_new.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o fat_tree_switch.o
	$(CC) $(CFLAGS) firstfit.o main_roce_new.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o $(LIB) -lhtsim -o htsim_roce_new

htsim_hpcc: main_hpcc.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o fat_tree_switch.o
	$(CC) $(CFLAGS) firstfit.o main_hpcc.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o $(LIB) -lhtsim -o htsim_hpcc

htsim_swift: main_swift.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o generic_topology.o
	$(CC) $(CFLAGS) firstfit.o main_swift.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o generic_topology.o $(LIB) -lhtsim -o htsim_swift

htsim_constcca: main_const.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o generic_topology.o
	$(CC) $(CFLAGS) firstfit.o main_const.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o generic_topology.o $(LIB) -lhtsim -o htsim_constcca

htsim_constcca_old: main_const_old.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o generic_topology.o
	$(CC) $(CFLAGS) firstfit.o main_const_old.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o generic_topology.o $(LIB) -lhtsim -o htsim_constcca_old

htsim_consterase: main_const_erase.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o generic_topology.o
	$(CC) $(CFLAGS) firstfit.o main_const_erase.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o generic_topology.o $(LIB) -lhtsim -o htsim_consterase

htsim_multi_dc: main_multi_dc_const_erase.o firstfit.o ../libhtsim.a vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o multihomed_fat_tree_topology.o star_topology.o generic_topology.o multi_datacenter_topology.o
	$(CC) $(CFLAGS) firstfit.o main_multi_dc_const_erase.o vl2_topology.o fat_tree_topology.o fat_tree_switch.o bcube_topology.o connection_matrix.o oversubscribed_fat_tree_topology.o shortflows.o star_topology.o multihomed_fat_tree_topology.o generic_topology.o multi_datacenter_topology.o $(LIB) -lhtsim -o htsim_multi_dc

main_tcp.o: main_tcp.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_tcp.cpp

main_ndp.o: main_ndp.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_ndp.cpp 

main_roce.o: main_roce.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_roce.cpp

main_roce_new.o: main_roce_new.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_roce_new.cpp

main_hpcc.o: main_hpcc.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_hpcc.cpp 

main_swift.o: main_swift.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_swift.cpp 

main_const.o: main_const.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_const.cpp 

main_const_old.o: main_const_old.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_const_old.cpp 

main_const_erase.o: main_const_erase.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_const_erase.cpp

main_multi_dc_const_erase.o: main_multi_dc_const_erase.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_multi_dc_const_erase.cpp

multi_datacenter_topology.o: multi_datacenter/multi_datacenter_topology.cpp multi_datacenter/multi_datacenter_topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c multi_datacenter/multi_datacenter_topology.cpp 

star_topology.o: star_topology.cpp star_topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c star_topology.cpp 

generic_topology.o: generic_topology.cpp generic_topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c generic_topology.cpp 

shortflows.o: shortflows.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c shortflows.cpp 

connection_matrix.o: connection_matrix.cpp bcube_topology.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c connection_matrix.cpp 

firstfit.o: firstfit.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c firstfit.cpp

vl2_topology.o: vl2_topology.cpp vl2_topology.h topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c vl2_topology.cpp

bcube_topology.o: bcube_topology.cpp bcube_topology.h topology.h ../matrix.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c bcube_topology.cpp

camcubetopology.o: camcubetopology.cpp camcubetopology.h topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c camcubetopology.cpp

subflow_control.o: subflow_control.cpp subflow_control.h  ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c subflow_control.cpp

fat_tree_topology.o: fat_tree_topology.cpp fat_tree_topology.h topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c fat_tree_topology.cpp

main_waterfill.o: main_waterfill.cpp connection_matrix.h connection_matrix.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c main_waterfill.cpp

dragon_fly_topology.o: dragon_fly_topology.cpp dragon_fly_topology.h topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c dragon_fly_topology.cpp

oversubscribed_fat_tree_topology.o: oversubscribed_fat_tree_topology.cpp oversubscribed_fat_tree_topology.h topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c oversubscribed_fat_tree_topology.cpp

multihomed_fat_tree_topology.o: multihomed_fat_tree_topology.cpp multihomed_fat_tree_topology.h topology.h ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c multihomed_fat_tree_topology.cpp

fat_tree_switch.o: fat_tree_switch.h fat_tree_switch.cpp ${DEPS}
	$(CC) $(INCLUDE) $(CFLAGS) -c fat_tree_switch.cpp

main_eqds.o: main_eqds.cpp
	$(CC) $(INCLUDE) $(CFLAGS) -c main_eqds.cpp 

clean:	
	rm -f *.o htsim_ndp* htsim_swift* htsim_tcp* htsim_dctcp* htsim_roce* htsim_hpcc* htsim_const* htsim_const_erase* htsim_const_old* htsim_roce_new*
