CXX ?= g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra

all: latency_sender latency_receiver

latency_sender: latency_sender.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

latency_receiver: latency_receiver.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f latency_sender latency_receiver

.PHONY: all clean
