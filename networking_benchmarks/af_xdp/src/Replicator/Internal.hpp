/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef PACKET_REPLICATOR_INTERNAL_HPP
#define PACKET_REPLICATOR_INTERNAL_HPP

/*
 * Shared internal include block for the Replicator implementation.
 *
 * Replicator's implementation is split across several cohesive translation
 * units (ReplicatorCore/Init/Groups/Control/Destinations/DataPath/Net.cpp).
 * They all share the same system-header surface and debug-print macros, so
 * that common preamble lives here — include this from every Replicator*.cpp
 * instead of repeating the block.
 */

#include "Replicator.hpp"

#include <stdexcept>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <ctime>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <errno.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

// Debug logging control - DISABLED for performance
#define DEBUG_TX 0
#define DEBUG_PACKET 0

#define DEBUG_TX_PRINT(fmt, ...) \
    do { \
        if (DEBUG_TX) { \
            std::cout << fmt << std::endl; \
        } \
    } while(0)

#define DEBUG_PACKET_PRINT(fmt, ...) \
    do { \
        if (DEBUG_PACKET) { \
            std::cout << fmt << std::endl; \
        } \
    } while(0)

#endif // PACKET_REPLICATOR_INTERNAL_HPP
