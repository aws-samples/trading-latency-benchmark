# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
#
# Native sender/receiver for the TGW multicast benchmark. Linux only for mcast_recv.
#
#   make                 build mcast_send and mcast_recv   (what instances use)
#   make synth           build mcast_recv_synth             (TEST ONLY: fakes the hardware stamp)
#   make stats_test      build and run the portable unit tests
#   make clean
#
# No -march flag: the instance gate allows both x86-64 (m7i, c7i...) and Graviton
# (m7g, c7g...), so the same source must build for either.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
BUILD    ?= build

all: $(BUILD)/mcast_send $(BUILD)/mcast_recv

$(BUILD)/mcast_send: mcast_send.cpp lat_stats.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD)/mcast_recv: mcast_recv.cpp lat_stats.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ mcast_recv.cpp

# TEST ONLY. Loopback and veth interfaces have no hardware timestamps; this build
# derives hw = sw - 700 ns so the rest of the pipeline can be exercised. Never ship it.
synth: $(BUILD)/mcast_recv_synth
$(BUILD)/mcast_recv_synth: mcast_recv.cpp lat_stats.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DMCAST_RECV_SYNTH_HW -o $@ mcast_recv.cpp

stats_test: $(BUILD)/stats_test
	$(BUILD)/stats_test
$(BUILD)/stats_test: tests/stats_test.cpp lat_stats.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/stats_test.cpp

clean:
	rm -rf $(BUILD)

.PHONY: all synth stats_test clean
