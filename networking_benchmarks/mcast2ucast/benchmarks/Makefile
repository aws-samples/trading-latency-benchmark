CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall

TARGETS = latency_test latency_sender latency_receiver

all: $(TARGETS)

latency_test: latency_test.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

latency_sender: latency_sender.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

latency_receiver: latency_receiver.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
