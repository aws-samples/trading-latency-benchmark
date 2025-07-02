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
#include "PacketReplicator.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <signal.h>
#include <unistd.h>

static std::unique_ptr<PacketReplicator> g_replicator;
static volatile bool g_running = true;

void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    g_running = false;
    if (g_replicator) {
        g_replicator->stop();
    }
}

void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " <interface> <listen_ip> <listen_port> [zero_copy]" << std::endl;
    std::cout << "  interface:   Network interface to bind to (e.g., eth0)" << std::endl;
    std::cout << "  listen_ip:   IP address to listen on" << std::endl;
    std::cout << "  listen_port: Port to listen on" << std::endl;
    std::cout << "  zero_copy:   'true' to enable zero-copy mode (default: true)" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  sudo " << progName << " eth0 192.168.1.100 8080" << std::endl;
    std::cout << "  sudo " << progName << " enp0s3 10.0.0.10 9000 false" << std::endl;
    std::cout << std::endl;
    std::cout << "The replicator will:" << std::endl;
    std::cout << "  1. Listen for UDP packets to the specified IP:PORT using AF_XDP" << std::endl;
    std::cout << "  2. Accept control commands on port 12345 to manage destinations" << std::endl;
    std::cout << "  3. Replicate received packets to all configured destinations" << std::endl;
    std::cout << std::endl;
    std::cout << "Control Protocol (port 12345):" << std::endl;
    std::cout << "  Add destination:    [1][4-byte IP][2-byte port]" << std::endl;
    std::cout << "  Remove destination: [2][4-byte IP][2-byte port]" << std::endl;
    std::cout << "  List destinations:  [3]" << std::endl;
}

void printStatisticsLoop() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (g_replicator && g_running) {
            g_replicator->printStatistics();
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4 || argc > 5) {
        printUsage(argv[0]);
        return 1;
    }

    // Check if running as root
    if (getuid() != 0) {
        std::cerr << "Error: This program must be run as root for AF_XDP access" << std::endl;
        std::cerr << "Please run with: sudo " << argv[0] << " ..." << std::endl;
        return 1;
    }

    std::string interface = argv[1];
    std::string listen_ip = argv[2];
    uint16_t listen_port = static_cast<uint16_t>(std::stoi(argv[3]));
    
    bool use_zero_copy = true;
    if (argc >= 5) {
        std::string zero_copy_str = argv[4];
        use_zero_copy = (zero_copy_str == "true" || zero_copy_str == "1");
    }

    std::cout << "=== AF_XDP Packet Replicator ===" << std::endl;
    std::cout << "Interface: " << interface << std::endl;
    std::cout << "Listen IP: " << listen_ip << std::endl;
    std::cout << "Listen Port: " << listen_port << std::endl;
    std::cout << "Zero Copy: " << (use_zero_copy ? "Enabled" : "Disabled") << std::endl;
    std::cout << "Control Port: " << PacketReplicator::CONTROL_PORT << std::endl;
    std::cout << "=================================" << std::endl;

    // Setup signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        // Create and initialize the replicator
        g_replicator = std::make_unique<PacketReplicator>(interface, listen_ip, listen_port);
        
        std::cout << "Initializing AF_XDP socket..." << std::endl;
        g_replicator->initialize(use_zero_copy);
        
        std::cout << "Starting packet replicator..." << std::endl;
        g_replicator->start();
        
        // Start statistics reporting thread
        std::thread stats_thread(printStatisticsLoop);
        
        std::cout << "Packet replicator is running!" << std::endl;
        std::cout << "Listening for UDP packets to " << listen_ip << ":" << listen_port << std::endl;
        std::cout << "Control protocol available on port " << PacketReplicator::CONTROL_PORT << std::endl;
        std::cout << "Press Ctrl+C to stop..." << std::endl;
        std::cout << std::endl;
        
        // Print initial help
        std::cout << "To add destinations, use the control client:" << std::endl;
        std::cout << "  ./control_client add <dest_ip> <dest_port>" << std::endl;
        std::cout << "  ./control_client list" << std::endl;
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
}
