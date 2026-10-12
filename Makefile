# Alexander Liu
# EECE 446 - Program 2: Peer-to-Peer Introduction
# Fall 2026

EXE = peer
SRC = peer.cpp

CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -fsanitize=address -static-libasan
LDLIBS =

.PHONY: all
all: $(EXE)

$(EXE): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) $(LDLIBS) -o $(EXE)

.PHONY: clean
clean:
	rm -f $(EXE)
