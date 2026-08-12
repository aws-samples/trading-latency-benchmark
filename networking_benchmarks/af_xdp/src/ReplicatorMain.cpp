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
#ifndef KERNEL_MODE_ONLY
#include "Replicator.hpp"
#endif
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <signal.h>
#include <unistd.h>

#ifndef KERNEL_MODE_ONLY
static std::unique_ptr<Replicator> g_replicator;
#endif
volatile bool g_running = true;

void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    g_running = false;
#ifndef KERNEL_MODE_ONLY
    if (g_replicator) {
        g_replicator->stop();
    }
#endif
}

// Forward declaration for kernel-mode echo server
extern int run_kernel_mode(const std::string& listen_ip, uint16_t listen_port);

void printUsage(const char* progName) {
    std::cout << "Usage: " << progName
              << " [--kernel-mode] <interface> <listen_ip> <listen_port> [zero_copy] [--gre]"
              << " [--ctrl <group>:<port>] [--producer <ip>:<port>]" << std::endl;
    std::cout << std::endl;
    std::cout << "  --kernel-mode Use standard UDP sockets instead of AF_XDP." << std::endl;
    std::cout << "                No root required. Works in containers and on macOS." << std::endl;
    std::cout << "                Usage: " << progName << " --kernel-mode <listen_ip> <listen_port>" << std::endl;
    std::cout << std::endl;
    std::cout << "  interface:    Network interface to bind to (e.g., eth0)" << std::endl;
    std::cout << "  listen_ip:    Unicast IP to listen on (unicast mode), or inner multicast" << std::endl;
    std::cout << "                group address (GRE mode, required)." << std::endl;
    std::cout << "  listen_port:  UDP data port." << std::endl;
    std::cout << "  zero_copy:    'true' to enable zero-copy mode (default: true)" << std::endl;
    std::cout << "  --gre:        GRE tunnel mode — outer unicast GRE carries inner multicast." << std::endl;
    std::cout << "                Loads mcast_filter.o; listen_ip is the inner multicast group." << std::endl;
    std::cout << "                Destinations register via CTRL_MCAST_JOIN (ctl mcast)." << std::endl;
    std::cout << "  --ctrl <g:p>  Multicast group:port where destinations send control messages." << std::endl;
    std::cout << "                Feeder joins this group and listens for control datagrams." << std::endl;
    std::cout << "                Requires --producer." << std::endl;
    std::cout << "  --producer <ip:port>" << std::endl;
    std::cout << "                Unicast endpoint of the upstream producer." << std::endl;
    std::cout << "                Control messages received from destinations are forwarded here." << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  # Unicast mode:" << std::endl;
    std::cout << "  sudo " << progName << " eth0 10.0.1.20 5000" << std::endl;
    std::cout << std::endl;
    std::cout << "  # GRE + upstream control forwarding:" << std::endl;
    std::cout << "  sudo " << progName
              << " eth0 224.0.31.50 5000 true --gre --ctrl 224.0.31.51:5001 --producer 10.0.1.10:6000" << std::endl;
    std::cout << std::endl;
    std::cout << "  # GRE only (no control forwarding):" << std::endl;
    std::cout << "  sudo " << progName << " eth0 224.0.31.50 5000 true --gre" << std::endl;
    std::cout << std::endl;
    std::cout << "Control Protocol (port 12345):" << std::endl;
    std::cout << "  Add destination:    [1][4-byte IP][2-byte port]" << std::endl;
    std::cout << "  Remove destination: [2][4-byte IP][2-byte port]" << std::endl;
    std::cout << "  List destinations:  [3]" << std::endl;
}

#ifndef KERNEL_MODE_ONLY
void printStatisticsLoop() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (g_replicator && g_running) {
            g_replicator->printStatistics();
        }
    }
}
#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // Kernel mode: simple UDP echo, no AF_XDP, no root required
    if (std::string(argv[1]) == "--kernel-mode") {
        if (argc < 4) {
            std::cerr << "Usage: " << argv[0] << " --kernel-mode <listen_ip> <listen_port>" << std::endl;
            return 1;
        }
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);
        std::string ip = argv[2];
        uint16_t port = static_cast<uint16_t>(std::stoi(argv[3]));
        return run_kernel_mode(ip, port);
    }

#ifdef KERNEL_MODE_ONLY
    std::cerr << "Error: This binary was built in kernel-mode-only configuration." << std::endl;
    std::cerr << "AF_XDP mode is not available. Use --kernel-mode flag." << std::endl;
    return 1;
#else
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    // Check if running as root
    if (getuid() != 0) {
        std::cerr << "Error: This program must be run as root for AF_XDP access" << std::endl;
        std::cerr << "Please run with: sudo " << argv[0] << " ..." << std::endl;
        std::cerr << "Or use --kernel-mode for testing without root." << std::endl;
        return 1;
    }

    std::string interface = argv[1];
    std::string listen_ip = argv[2];
    uint16_t listen_port = static_cast<uint16_t>(std::stoi(argv[3]));

    bool use_zero_copy = true;
    bool use_gre = false;
    std::string ctrl_group;
    uint16_t    ctrl_port = 0;
    std::string producer_ip;
    uint16_t    producer_port = 0;
    // AF_XDP queue count. Hardcoded to 1 — single RSS queue receives all traffic
    // from a given 5-tuple. Multiple queues only useful with multiple distinct
    // inbound sources (different src IPs). To re-enable: add --queues CLI flag
    // and pass to Replicator constructor.
    int         num_queues = 1;

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--gre") {
            use_gre = true;
        } else if (arg == "--ctrl" && i + 1 < argc) {
            // Format: <group>:<port>
            std::string val = argv[++i];
            auto colon = val.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "Error: --ctrl expects <group>:<port>" << std::endl;
                return 1;
            }
            ctrl_group = val.substr(0, colon);
            ctrl_port  = static_cast<uint16_t>(std::stoi(val.substr(colon + 1)));
        } else if (arg == "--queues" && i + 1 < argc) {
            // Stub: reserved for future multi-queue support. Currently ignored.
            // num_queues = std::stoi(argv[++i]);
            ++i; // consume arg
            std::cerr << "Warning: --queues is reserved for future use (fixed at 1)" << std::endl;
        } else if (arg == "--producer" && i + 1 < argc) {
            // Format: <ip>:<port>
            std::string val = argv[++i];
            auto colon = val.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "Error: --producer expects <ip>:<port>" << std::endl;
                return 1;
            }
            producer_ip   = val.substr(0, colon);
            producer_port = static_cast<uint16_t>(std::stoi(val.substr(colon + 1)));
        } else {
            use_zero_copy = (arg == "true" || arg == "1");
        }
    }

    if ((!ctrl_group.empty()) != (!producer_ip.empty())) {
        std::cerr << "Error: --ctrl and --producer must be used together" << std::endl;
        return 1;
    }

    std::cout << "=== AF_XDP Packet Replicator ===" << std::endl;
    std::cout << "Interface:    " << interface << std::endl;
    std::cout << "Listen IP:    " << listen_ip << ":" << listen_port << std::endl;
    std::cout << "Zero Copy:    " << (use_zero_copy ? "Enabled" : "Disabled") << std::endl;
    std::cout << "Mode:         " << (use_gre ? "GRE tunnel" : "Unicast") << std::endl;
    if (!ctrl_group.empty()) {
        std::cout << "Ctrl group:   " << ctrl_group << ":" << ctrl_port << std::endl;
        std::cout << "Producer:     " << producer_ip << ":" << producer_port << std::endl;
    }
    std::cout << "Control Port: " << Replicator::CONTROL_PORT << std::endl;
    std::cout << "=================================" << std::endl;

    // Setup signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        // Create and initialize the replicator
        g_replicator = std::make_unique<Replicator>(interface, listen_ip, listen_port, num_queues);

        if (use_gre) {
            g_replicator->setGREMode(true);
        }

        if (!ctrl_group.empty()) {
            g_replicator->setUpstreamControl(ctrl_group, ctrl_port, producer_ip, producer_port);
        }

        std::cout << "Initializing AF_XDP socket..." << std::endl;
        g_replicator->initialize(use_zero_copy);
        
        std::cout << "Starting packet replicator..." << std::endl;
        g_replicator->start();
        
        // Start statistics reporting thread
        std::thread stats_thread(printStatisticsLoop);
        
        std::cout << "Packet replicator is running!" << std::endl;
        std::cout << "Control protocol available on port " << Replicator::CONTROL_PORT << std::endl;
        std::cout << "Press Ctrl+C to stop..." << std::endl;
        std::cout << std::endl;
        
        // Print initial help
        std::cout << "To add destinations, use the control client:" << std::endl;
        std::cout << "  ./ctl add <dest_ip> <dest_port>" << std::endl;
        std::cout << "  ./ctl list" << std::endl;
        std::cout << std::endl;
        
        // Main loop - just wait for signal
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // Check if replicator is still running
            if (!g_replicator->isRunning()) {
                std::cerr << "Replicator stopped unexpectedly" << std::endl;
                g_running = false;
                break;
            }
        }
        
        std::cout << "Stopping replicator..." << std::endl;
        g_replicator->stop();
        
        // Wait for stats thread to finish
        if (stats_thread.joinable()) {
            stats_thread.join();
        }
        
        // Print final statistics
        std::cout << "\nFinal Statistics:" << std::endl;
        g_replicator->printStatistics();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Packet replicator stopped" << std::endl;
    return 0;
#endif  // KERNEL_MODE_ONLY
}
