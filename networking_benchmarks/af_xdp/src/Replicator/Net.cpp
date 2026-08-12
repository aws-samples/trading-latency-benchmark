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

// ReplicatorNet.cpp — network address helpers: IP string <-> NBO conversion,
// interface IP/MAC discovery (ioctl), and ARP/gateway resolution driven off
// /proc/net/arp and /proc/net/route.

#include "Internal.hpp"

uint32_t Replicator::parseIpAddress(const std::string& ipStr) {
    struct in_addr addr;
    if (inet_aton(ipStr.c_str(), &addr) == 0) {
        throw std::invalid_argument("Invalid IP address: " + ipStr);
    }
    return addr.s_addr; // Already in network byte order
}

std::string Replicator::formatIpAddress(uint32_t ipAddr) {
    struct in_addr addr;
    addr.s_addr = ipAddr;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return std::string(buf);
}

bool Replicator::getInterfaceIp(const std::string& interface, std::string& ip_address) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
        close(sock);
        return false;
    }

    close(sock);

    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
    ip_address = buf;
    return true;
}

bool Replicator::getInterfaceMac(const std::string& interface, uint8_t* mac_address) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        close(sock);
        return false;
    }

    close(sock);

    memcpy(mac_address, ifr.ifr_hwaddr.sa_data, 6);
    return true;
}

// Scan /proc/net/arp for a single IP entry and return its MAC.
static bool lookupArpEntry(const std::string& ip_address, uint8_t* mac_address) {
    std::ifstream arp_file("/proc/net/arp");
    if (!arp_file.is_open()) return false;

    std::string line;
    std::getline(arp_file, line); // skip header
    while (std::getline(arp_file, line)) {
        std::istringstream iss(line);
        std::string ip, hw_type, flags, mac, mask, device;
        if (!(iss >> ip >> hw_type >> flags >> mac >> mask >> device)) continue;
        if (ip != ip_address || mac == "00:00:00:00:00:00") continue;
        int v[6];
        if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x",
                   &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) continue;
        for (int i = 0; i < 6; i++) mac_address[i] = (uint8_t)v[i];
        return true;
    }
    return false;
}

// Read the gateway IP for a given destination from /proc/net/route.
// Returns the most-specific matching gateway (longest mask).
static bool lookupGateway(const std::string& dest_ip, std::string& gateway_ip) {
    std::ifstream route_file("/proc/net/route");
    if (!route_file.is_open()) return false;

    std::string line;
    std::getline(route_file, line); // skip header

    uint32_t best_gw   = 0;
    uint32_t best_mask = 0xFFFFFFFF; // sentinel: no match yet
    bool     found     = false;
    uint32_t dest_addr = inet_addr(dest_ip.c_str());

    while (std::getline(route_file, line)) {
        std::istringstream iss(line);
        std::string iface;
        uint32_t destination, gateway, flags, mask;
        int ref, use, metric;
        if (!(iss >> iface
                  >> std::hex >> destination >> gateway >> flags
                  >> std::dec >> ref >> use >> metric
                  >> std::hex >> mask))
            continue;
        if (!(flags & 0x1)) continue; // RTF_UP
        if (!(flags & 0x2)) continue; // RTF_GATEWAY
        if ((dest_addr & mask) != destination) continue;
        // Prefer longer (more specific) prefix
        if (!found || mask > best_mask) {
            best_gw   = gateway;
            best_mask = mask;
            found     = true;
        }
    }

    if (!found) return false;
    struct in_addr gw{}; gw.s_addr = best_gw;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &gw, buf, sizeof(buf));
    gateway_ip = buf;
    return true;
}

bool Replicator::getDestinationMac(const std::string& ip_address, uint8_t* mac_address) {
    // Fast path: destination is directly reachable (same subnet)
    if (lookupArpEntry(ip_address, mac_address)) return true;

    // Off-subnet destination (e.g. cross-VPC via peering): the kernel routes
    // via the local gateway.  Use the gateway's MAC so ENA delivers the frame
    // to the VPC router, which forwards it over the peering connection.
    std::string gateway_ip;
    if (lookupGateway(ip_address, gateway_ip)) {
        if (lookupArpEntry(gateway_ip, mac_address)) {
            std::cout << "Off-subnet " << ip_address
                      << " — using gateway " << gateway_ip << " MAC" << std::endl;
            return true;
        }
    }

    return false;
}

void Replicator::triggerArpResolution(const std::string& ip_address) {
    std::cout << "Triggering ARP resolution for " << ip_address << std::endl;
    
    // Create a temporary socket to send a UDP packet to trigger ARP resolution
    int temp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (temp_socket < 0) {
        std::cerr << "Failed to create temp socket for ARP resolution: " << strerror(errno) << std::endl;
        return;
    }
    
    // Create destination address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(12346);  // Use a different port to avoid conflicts
    
    if (inet_aton(ip_address.c_str(), &dest_addr.sin_addr) == 0) {
        std::cerr << "Invalid IP address for ARP resolution: " << ip_address << std::endl;
        close(temp_socket);
        return;
    }
    
    // Send a small UDP packet to trigger ARP resolution
    const char* arp_trigger_message = "ARP";
    ssize_t sent = sendto(temp_socket, arp_trigger_message, strlen(arp_trigger_message), 0,
                         (const struct sockaddr*)&dest_addr, sizeof(dest_addr));
    
    if (sent < 0) {
        std::cerr << "Failed to send ARP trigger packet to " << ip_address << ": " << strerror(errno) << std::endl;
    } else {
        // Poll /proc/net/arp at 1ms intervals instead of a fixed sleep.
        // AWS VPC same-AZ ARP resolves in ~3-5ms; cross-AZ up to ~15ms.
        // Cap at 100ms — if unresolved by then, addDestination() stores broadcast MAC
        // and the self-healing path in updateDestinationCache() retries every 100ms.
        static constexpr int POLL_INTERVAL_MS = 3;
        static constexpr int MAX_WAIT_MS      = 100;
        uint8_t mac[6];
        bool resolved = false;
        for (int elapsed = 0; elapsed < MAX_WAIT_MS; elapsed += POLL_INTERVAL_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
            if (getDestinationMac(ip_address, mac)) {
                resolved = true;
                break;
            }
        }
        if (resolved) {
            std::cout << "ARP resolved for " << ip_address << " MAC: " << std::hex
                      << (int)mac[0] << ":" << (int)mac[1] << ":" << (int)mac[2] << ":"
                      << (int)mac[3] << ":" << (int)mac[4] << ":" << (int)mac[5]
                      << std::dec << std::endl;
        } else {
            std::cout << "ARP not resolved within " << MAX_WAIT_MS << "ms for " << ip_address
                      << " — broadcast MAC used until cache refresh" << std::endl;
        }
    }
    
    close(temp_socket);
}
