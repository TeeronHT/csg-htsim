# Model-validity dumbbell runner. One process per simulation.
# Links the existing simulator; does not rebuild the ring driver.

SIM := ../../../sim
CXX := g++
CXXFLAGS := -Wall -std=c++11 -Wsign-compare -O2
INCLUDES := -I$(SIM) -Isrc

.PHONY: all clean

all: htsim_mv

htsim_mv: src/main_mv.cpp $(SIM)/libhtsim.a
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ src/main_mv.cpp -L$(SIM) -lhtsim

$(SIM)/libhtsim.a:
	$(MAKE) -C $(SIM) libhtsim.a

clean:
	rm -rf htsim_mv htsim_mv.dSYM
