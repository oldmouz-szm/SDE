CXX = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra
LIBS = -lminisat -pthread

all: sde_solver obs_generator cda_solver

sde_solver: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

obs_generator: obs_generator.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

cda_solver: cda_solver.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f sde_solver obs_generator cda_solver
