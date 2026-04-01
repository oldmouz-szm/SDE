CXX = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra
LIBS = -lminisat -pthread

all: sde_solver obs_generator

sde_solver: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

obs_generator: obs_generator.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f sde_solver obs_generator
