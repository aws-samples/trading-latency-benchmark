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

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

static bool isMulticastAddress(const std::string& ip) {
    struct in_addr addr{};
    if (inet_aton(ip.c_str(), &addr) == 0) return false;
    // 224.0.0.0/4: first octet 224–239
    return (ntohl(addr.s_addr) >> 28) == 0xE;
}

static volatile bool g_running = true;

void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ", stopping..." << std::endl;
    g_running = false;
}

void printUsage(const char* progName) {
    std::cout << "Usage: " << progName
              << " <target_ip> <target_port> [interval_ms] [message] [--iface <name>]" << std::endl;
    std::cout << "  target_ip:    IP address to send UDP packets to." << std::endl;
    std::cout << "                Use a multicast group (e.g. 224.0.31.50) for:" << std::endl;
    std::cout << "                  GRE mode   — kernel routes via 'ip route add 224.0.0.0/4 dev gre_feed'" << std::endl;
    std::cout << "                  Native mcast — direct multicast on the LAN/TGW domain" << std::endl;
    std::cout << "  target_port:  UDP destination port." << std::endl;
    std::cout << "  interval_ms:  Interval between packets in milliseconds (default: 1000)." << std::endl;
    std::cout << "  message:      Payload string (default: 'Test packet')." << std::endl;
    std::cout << "  --iface <n>   Outgoing interface for multicast (sets IP_MULTICAST_IF)." << std::endl;
    std::cout << "                Required for native multicast; optional for GRE mode." << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  # Unicast to feeder:" << std::endl;
    std::cout << "  " << progName << " 10.0.1.20 5000" << std::endl;
    std::cout << "  # GRE mode (kernel GRE tunnel must be configured on this host):" << std::endl;
    std::cout << "  " << progName << " 224.0.31.50 5000 100 'trade'  --iface eth0" << std::endl;
    std::cout << "  # Native multicast:" << std::endl;
    std::cout << "  " << progName << " 224.0.31.50 5000 100 'trade'  --iface eth0" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    std::string target_ip   = argv[1];
    uint16_t    target_port = static_cast<uint16_t>(std::stoi(argv[2]));
    int         interval_ms = 1000;
    std::string message     = "Test packet";
    std::string iface_name;

    // Parse positional args 3 & 4, then flags
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--iface" && i + 1 < argc) {
            iface_name = argv[++i];
        } else if (i == 3 && arg.find_first_not_of("0123456789") == std::string::npos) {
            interval_ms = std::stoi(arg);
        } else if (i == 4 && arg != "--iface") {
            message = arg;
        } else if (arg != "--iface") {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    bool multicast = isMulticastAddress(target_ip);

    std::cout << "=== UDP Test Client ===" << std::endl;
    std::cout << "Target:   " << target_ip << ":" << target_port << std::endl;
    std::cout << "Mode:     " << (multicast ? "multicast" : "unicast") << std::endl;
    if (!iface_name.empty())
        std::cout << "Iface:    " << iface_name << std::endl;
    std::cout << "Interval: " << interval_ms << " ms" << std::endl;
    std::cout << "Message:  '" << message << "'" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << "======================" << std::endl;

    // Setup signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    try {
        // Create UDP socket
        int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) {
            throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
        }

        // Multicast sender configuration
        if (multicast) {
            // TTL=8: crosses TGW/VPC boundaries but stays within a reasonable scope
            int ttl = 8;
            if (setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
                close(socket_fd);
                throw std::runtime_error("Failed to set IP_MULTICAST_TTL: " + std::string(strerror(errno)));
            }

            // Bind outgoing multicast to specific interface if requested.
            // Required for native multicast (prevents sending on the wrong interface).
            // In GRE mode the kernel route handles it, but specifying is still cleaner.
            if (!iface_name.empty()) {
                struct ip_mreqn mreqn{};
                mreqn.imr_ifindex = static_cast<int>(if_nametoindex(iface_name.c_str()));
                if (mreqn.imr_ifindex == 0) {
                    close(socket_fd);
                    throw std::runtime_error("Unknown interface: " + iface_name);
                }
                if (setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &mreqn, sizeof(mreqn)) < 0) {
                    close(socket_fd);
                    throw std::runtime_error("Failed to set IP_MULTICAST_IF: " + std::string(strerror(errno)));
                }
            }
        }

        // Setup destination address
        struct sockaddr_in dest_addr;
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(target_port);

        if (inet_aton(target_ip.c_str(), &dest_addr.sin_addr) == 0) {
            close(socket_fd);
            throw std::runtime_error("Invalid target IP address: " + target_ip);
        }

        uint64_t packet_count = 0;
        auto start_time = std::chrono::steady_clock::now();

        std::cout << "Starting to send UDP packets..." << std::endl;

        while (g_running) {
            // Create packet with sequence number
            std::string packet_data = message + " #" + std::to_string(packet_count + 1);
            
            // Send the packet
            ssize_t sent = sendto(socket_fd, packet_data.c_str(), packet_data.length(), 0,
                                 (struct sockaddr*)&dest_addr, sizeof(dest_addr));

            if (sent < 0) {
                if (g_running) {
                    std::cerr << "Failed to send packet: " << strerror(errno) << std::endl;
                }
                break;
            }

            packet_count++;
            
            // Print progress every 10 packets or when interval is large
            if (packet_count % 10 == 0 || interval_ms >= 1000) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                
                std::cout << "Sent packet #" << packet_count << " (" << sent << " bytes) "
                          << "- Running for " << elapsed << "s" << std::endl;
            }

            // Wait for next packet
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }

        close(socket_fd);

        // Print final statistics
        auto end_time = std::chrono::steady_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        
        std::cout << std::endl;
        std::cout << "=== Final Statistics ===" << std::endl;
        std::cout << "Packets sent: " << packet_count << std::endl;
        std::cout << "Total time: " << total_time << " ms" << std::endl;
        if (total_time > 0) {
            double rate = (packet_count * 1000.0) / total_time;
            std::cout << "Average rate: " << std::fixed << std::setprecision(2) << rate << " packets/sec" << std::endl;
        }
        std::cout << "========================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Test client stopped" << std::endl;
    return 0;
}
