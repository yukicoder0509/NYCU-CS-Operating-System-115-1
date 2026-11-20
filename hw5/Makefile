CXX      = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17

all: main

main: main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

# Usage: make run T=path/to/trace.txt
T ?= tests/small.txt
run: main
	./main $(T)

clean:
	rm -f main

.PHONY: all run clean
