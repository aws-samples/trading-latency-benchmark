// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: MIT-0
//
// Test helper: read signed nanosecond values (one per line) from stdin and print the
// SignedHist wire form that mcast_recv writes to its detail file. Lets the Python
// pooling code be checked against the real C++ implementation.
#include <cstdio>
#include <iostream>

#include "../lat_stats.hpp"

int main() {
    latstats::SignedHist h;
    long long v;
    while (std::cin >> v) h.add(v);
    std::printf("%s\n", h.to_json().c_str());
    return 0;
}
