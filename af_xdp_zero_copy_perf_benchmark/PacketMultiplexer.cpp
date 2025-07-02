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

#include "PacketMultiplexer.hpp"
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/ether.h>
#include <arpa/inet.h>
#include <errno.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

// Destination implementation
PacketMultiplexer::Destination::Destination(const std::string& ip, uint16_t p) 
    : ip_address(ip), port(p) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(ip_address.c_str(), &addr.sin_addr) == 0) {
        throw std::invalid_argument("Invalid IP address: " + ip_address);
    }
}

bool PacketMultiplexer::Destination::operator<(const Destination& other) const {
    if (ip_address != other.ip_address) {
        return ip_address < other.ip_address;
    }
    return port < other.port;
}

// PacketMultiplexer implementation
PacketMultiplexer::PacketMultiplexer(const std::string& interface, const std::string& listenIp, uint16_t listenPort)
    : listen_interface_(interface), listen_ip_(listenIp), listen_port_(listenPort),
      num_queues_(4), output_xdp_socket_(nullptr), control_socket_(-1), output_socket_(-1), running_(false),
      packets_received_(0), packets_sent_(0), bytes_received_(0), bytes_sent_(0) {
    
    // Initialize per-queue statistics
    for (int i = 0; i < MAX_QUEUES; i++) {
        packets_received_per_queue_[i].store(0);
        packets_sent_per_queue_[i].store(0);
    }
    
    std::cout << "PacketMultiplexer initializing for " << listen_ip_ << ":" << listen_port_ 
              << " on interface " << listen_interface_ << " with " << num_queues_ << " queues" << std::endl;
}

PacketMultiplexer::~PacketMultiplexer() {
    stop();
    
    if (control_socket_ >= 0) {
        ::close(control_socket_);
    }
    if (output_socket_ >= 0) {
        ::close(output_socket_);
    }
}

PacketMultiplexer::PacketMultiplexer(PacketMultiplexer&& other) noexcept
    : listen_interface_(std::move(other.listen_interface_)),
      listen_ip_(std::move(other.listen_ip_)),
      listen_port_(other.listen_port_),
      num_queues_(other.num_queues_),
      xdp_sockets_(std::move(other.xdp_sockets_)),
      control_socket_(other.control_socket_),
      output_socket_(other.output_socket_),
      running_(other.running_.load()),
      packet_processor_threads_(std::move(other.packet_processor_threads_)),
      control_thread_(std::move(other.control_thread_)),
      destinations_(std::move(other.destinations_)),
      packets_received_(other.packets_received_.load()),
      packets_sent_(other.packets_sent_.load()),
      bytes_received_(other.bytes_received_.load()),
      bytes_sent_(other.bytes_sent_.load()) {
    
    // Copy per-queue statistics arrays
    for (int i = 0; i < MAX_QUEUES; i++) {
        packets_received_per_queue_[i].store(other.packets_received_per_queue_[i].load());
        packets_sent_per_queue_[i].store(other.packets_sent_per_queue_[i].load());
    }
    
    other.control_socket_ = -1;
    other.output_socket_ = -1;
    other.running_ = false;
}

PacketMultiplexer& PacketMultiplexer::operator=(PacketMultiplexer&& other) noexcept {
    if (this != &other) {
        stop();
        
        listen_interface_ = std::move(other.listen_interface_);
        listen_ip_ = std::move(other.listen_ip_);
        listen_port_ = other.listen_port_;
        num_queues_ = other.num_queues_;
        xdp_sockets_ = std::move(other.xdp_sockets_);
        control_socket_ = other.control_socket_;
        output_socket_ = other.output_socket_;
        running_ = other.running_.load();
        packet_processor_threads_ = std::move(other.packet_processor_threads_);
        control_thread_ = std::move(other.control_thread_);
        destinations_ = std::move(other.destinations_);
        packets_received_ = other.packets_received_.load();
        packets_sent_ = other.packets_sent_.load();
        bytes_received_ = other.bytes_received_.load();
        bytes_sent_ = other.bytes_sent_.load();
        
        // Copy per-queue statistics arrays
        for (int i = 0; i < MAX_QUEUES; i++) {
            packets_received_per_queue_[i].store(other.packets_received_per_queue_[i].load());
            packets_sent_per_queue_[i].store(other.packets_sent_per_queue_[i].load());
        }
        
        other.control_socket_ = -1;
        other.output_socket_ = -1;
        other.running_ = false;
    }
    return *this;
}

void PacketMultiplexer::initialize(bool useZeroCopy) {
    std::cout << "Initializing PacketMultiplexer with zero-copy: " << (useZeroCopy ? "enabled" : "disabled") << std::endl;
    
    // Set resource limits for AF_XDP
    AFXDPSocket::setResourceLimits();
    
    // Create AF_XDP sockets for all queues
    xdp_sockets_.resize(num_queues_);
    
    // Load XDP program once
    std::string xdp_program_path = "./unicast_filter.o";
    AFXDPSocket::loadXdpProgram(listen_interface_, xdp_program_path, useZeroCopy);
    
    // Configure XDP program with our target IP and port
    configureXdpProgram();
    
    // Create and configure AF_XDP socket for each queue
    int xdp_flags = useZeroCopy ? AFXDPSocket::XDP_FLAGS_ZERO_COPY : AFXDPSocket::XDP_FLAGS_DRV_MODE;
    
    for (int queue_id = 0; queue_id < num_queues_; queue_id++) {
        std::cout << "Creating AF_XDP socket for queue " << queue_id << std::endl;
        
        // Create AF_XDP socket for this queue with proper frame count (following ena-xdp)
        xdp_sockets_[queue_id] = std::make_unique<AFXDPSocket>(4096, AFXDPSocket::DEFAULT_UMEM_FRAMES, 0);
        
        // Setup UMEM
        xdp_sockets_[queue_id]->setupUMem();
        
        // Bind socket to this specific queue
        xdp_sockets_[queue_id]->bind(listen_interface_, queue_id, xdp_flags);
        
        // Register with XDP map for this queue
        xdp_sockets_[queue_id]->registerXskMap(queue_id);
        
        std::cout << "AF_XDP socket for queue " << queue_id << " initialized successfully" << std::endl;
    }
    
    // Create control socket
    control_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (control_socket_ < 0) {
        throw std::runtime_error("Failed to create control socket: " + std::string(strerror(errno)));
    }
    
    // Bind control socket
    struct sockaddr_in control_addr;
    memset(&control_addr, 0, sizeof(control_addr));
    control_addr.sin_family = AF_INET;
    control_addr.sin_addr.s_addr = INADDR_ANY;
    control_addr.sin_port = htons(CONTROL_PORT);
    
    int opt = 1;
    if (setsockopt(control_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        throw std::runtime_error("Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
    }
    
    if (bind(control_socket_, (struct sockaddr*)&control_addr, sizeof(control_addr)) < 0) {
        throw std::runtime_error("Failed to bind control socket: " + std::string(strerror(errno)));
    }
    
    // Create output socket for sending to destinations
    output_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (output_socket_ < 0) {
        throw std::runtime_error("Failed to create output socket: " + std::string(strerror(errno)));
    }
    
    std::cout << "PacketMultiplexer initialized successfully with " << num_queues_ << " queues" << std::endl;
}

void PacketMultiplexer::configureXdpProgram() {
    // Find the config map in the loaded XDP program
    // This is a simplified approach - in production you'd want better error handling
    
    int config_map_fd = -1;
    
    // Try to find the config map by iterating through all BPF maps
    // This is a bit of a hack, but works for our demo
    struct bpf_map_info map_info = {};
    uint32_t info_len = sizeof(map_info);
    
    // Search for the config map - this is not the most elegant solution
    // but works for demonstration purposes
    for (int fd = 3; fd < 1024; fd++) {
        if (bpf_obj_get_info_by_fd(fd, &map_info, &info_len) == 0) {
            if (strstr(map_info.name, "config_map") != nullptr) {
                config_map_fd = fd;
                break;
            }
        }
    }
    
    if (config_map_fd < 0) {
        std::cout << "Warning: Could not find config_map, XDP program will pass all packets" << std::endl;
        return;
    }
    
    // Configure the XDP program with our target IP and port
    struct unicast_config {
        uint32_t target_ip;    // Target IP address in network byte order
        uint16_t target_port;  // Target port in network byte order
        uint16_t padding;      // Padding for alignment
    } config;
    
    config.target_ip = parseIpAddress(listen_ip_);
    config.target_port = htons(listen_port_);
    config.padding = 0;
    
    uint32_t key = 0;
    int ret = bpf_map_update_elem(config_map_fd, &key, &config, BPF_ANY);
    if (ret != 0) {
        std::cout << "Warning: Failed to update XDP config map: " << strerror(-ret) << std::endl;
    } else {
        std::cout << "Configured XDP program to filter packets for " << listen_ip_ << ":" << listen_port_ << std::endl;
    }
}

void PacketMultiplexer::addDestination(const std::string& ipAddress, uint16_t port) {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    
    Destination dest(ipAddress, port);
    destinations_.insert(dest);
    
    std::cout << "Added destination: " << ipAddress << ":" << port << std::endl;
    
    // Trigger ARP resolution to ensure MAC address is available
    triggerArpResolution(ipAddress);
}

void PacketMultiplexer::removeDestination(const std::string& ipAddress, uint16_t port) {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    
    Destination dest(ipAddress, port);
    destinations_.erase(dest);
    
    std::cout << "Removed destination: " << ipAddress << ":" << port << std::endl;
}

std::vector<PacketMultiplexer::Destination> PacketMultiplexer::getDestinations() const {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    return std::vector<Destination>(destinations_.begin(), destinations_.end());
}

void PacketMultiplexer::start() {
    if (!running_.exchange(true)) {
        std::cout << "Starting PacketMultiplexer..." << std::endl;
        
        // Start packet processing threads for each queue
        packet_processor_threads_.resize(num_queues_);
        for (int queue_id = 0; queue_id < num_queues_; queue_id++) {
            packet_processor_threads_[queue_id] = std::make_unique<std::thread>(
                [this, queue_id]() { this->processPacketsForQueue(queue_id); }
            );
            std::cout << "Started packet processing thread for queue " << queue_id << std::endl;
        }
        
        // Start control protocol thread
        control_thread_ = std::make_unique<std::thread>(&PacketMultiplexer::handleControlProtocol, this);
        
        std::cout << "PacketMultiplexer started with " << num_queues_ << " processing threads" << std::endl;
    }
}

void PacketMultiplexer::stop() {
    if (running_.exchange(false)) {
        std::cout << "Stopping PacketMultiplexer..." << std::endl;
        
        // Wait for all packet processor threads to finish
        for (auto& thread : packet_processor_threads_) {
            if (thread && thread->joinable()) {
                thread->join();
            }
        }
        packet_processor_threads_.clear();
        
        // Wait for control thread to finish
        if (control_thread_ && control_thread_->joinable()) {
            control_thread_->join();
            control_thread_.reset();
        }
        
        // Unload XDP program
        AFXDPSocket::unloadXdpProgram(listen_interface_, true);
        
        std::cout << "PacketMultiplexer stopped" << std::endl;
    }
}

bool PacketMultiplexer::isRunning() const {
    return running_.load();
}

PacketMultiplexer::Statistics PacketMultiplexer::getStatistics() const {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    return {
        packets_received_.load(),
        packets_sent_.load(),
        bytes_received_.load(),
        bytes_sent_.load(),
        destinations_.size()
    };
}

void PacketMultiplexer::printStatistics() const {
    auto stats = getStatistics();
    std::cout << "=== PacketMultiplexer Statistics ===" << std::endl;
    std::cout << "Packets received: " << stats.packets_received << std::endl;
    std::cout << "Packets sent: " << stats.packets_sent << std::endl;
    std::cout << "Bytes received: " << stats.bytes_received << std::endl;
    std::cout << "Bytes sent: " << stats.bytes_sent << std::endl;
    std::cout << "Active destinations: " << stats.destinations_count << std::endl;
    std::cout << "=================================" << std::endl;
}

// Removed processPackets() method - using processPacketsForQueue() instead

void PacketMultiplexer::processPacketsForQueue(int queueId) {
    std::cout << "Packet processing thread started for queue " << queueId << std::endl;
    
    std::vector<int> offsets(64);  // Batch size of 64 packets
    std::vector<int> lengths(64);
    
    while (running_) {
        try {
            // Receive packets from AF_XDP socket for this specific queue
            int received = xdp_sockets_[queueId]->receive(offsets, lengths);
            
            if (received > 0) {
                std::cout << "Queue " << queueId << ": Received " << received << " packets from XDP" << std::endl;
                
                // Process each packet
                for (int i = 0; i < received; i++) {
                    uint8_t* packet_data = xdp_sockets_[queueId]->getUmemBuffer() + offsets[i];
                    size_t packet_len = lengths[i];
                    
                    // Update per-queue and total statistics
                    packets_received_per_queue_[queueId]++;
                    packets_received_++;
                    bytes_received_ += packet_len;
                    
                    // Multiplex the packet to all destinations
                    int sent_count = multiplexPacket(packet_data, packet_len, queueId);
                    
                    if (sent_count > 0) {
                        packets_sent_per_queue_[queueId] += sent_count;
                        std::cout << "Queue " << queueId << ": Multiplexed packet to " << sent_count << " destinations" << std::endl;
                    }
                }
                
                // Recycle the frames back to the fill queue
                xdp_sockets_[queueId]->recycleFrames();
            }
        } catch (const std::exception& e) {
            if (running_) {
                std::cerr << "Error in packet processing for queue " << queueId << ": " << e.what() << std::endl;
            }
        }
        
        // Small delay to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    std::cout << "Packet processing thread stopped for queue " << queueId << std::endl;
}

void PacketMultiplexer::handleControlProtocol() {
    std::cout << "Control protocol thread started on port " << CONTROL_PORT << std::endl;
    
    std::vector<uint8_t> buffer(1024);
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(control_socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << "Failed to set control socket timeout: " << strerror(errno) << std::endl;
    }
    
    while (running_) {
        ssize_t bytes_received = recvfrom(control_socket_, buffer.data(), buffer.size(), 0,
                                         (struct sockaddr*)&client_addr, &addr_len);
        
        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; // Timeout - check if still running
            }
            if (running_) {
                std::cerr << "Error receiving control message: " << strerror(errno) << std::endl;
            }
            continue;
        }
        
        if (bytes_received > 0) {
            // Process control message
            auto response = processControlMessage(buffer.data(), bytes_received, client_addr);
            
            // Send response if needed
            if (!response.empty()) {
                sendto(control_socket_, response.data(), response.size(), 0,
                       (struct sockaddr*)&client_addr, addr_len);
            }
        }
    }
    
    std::cout << "Control protocol thread stopped" << std::endl;
}

int PacketMultiplexer::multiplexPacket(const uint8_t* packetData, size_t packetLen, int queueId) {
    // Extract UDP payload from the packet
    const uint8_t* payload_data = nullptr;
    size_t payload_len = 0;
    
    if (!extractUdpPayload(packetData, packetLen, payload_data, payload_len)) {
        return 0; // Not a valid UDP packet
    }
    
    // Get current destinations
    std::vector<Destination> current_destinations;
    {
        std::lock_guard<std::mutex> lock(destinations_mutex_);
        current_destinations = std::vector<Destination>(destinations_.begin(), destinations_.end());
    }
    
    if (current_destinations.empty()) {
        return 0; // No destinations to send to
    }
    
    // Send to all destinations using the same queue's socket to avoid race conditions
    int sent_count = 0;
    for (const auto& dest : current_destinations) {
        if (sendToDestinationWithQueue(dest, payload_data, payload_len, queueId)) {
            sent_count++;
            packets_sent_++;
            bytes_sent_ += payload_len;
        }
    }
    
    return sent_count;
}

bool PacketMultiplexer::extractUdpPayload(const uint8_t* packetData, size_t packetLen,
                                         const uint8_t*& payloadData, size_t& payloadLen) {
    // Minimum packet size check
    if (packetLen < sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr)) {
        return false;
    }
    
    const struct ethhdr* eth = (const struct ethhdr*)packetData;
    if (ntohs(eth->h_proto) != ETH_P_IP) {
        return false;
    }
    
    const struct iphdr* ip = (const struct iphdr*)(packetData + sizeof(struct ethhdr));
    if (ip->protocol != IPPROTO_UDP) {
        return false;
    }
    
    // Calculate IP header length
    size_t ip_hdr_len = ip->ihl * 4;
    if (ip_hdr_len < 20) {
        return false;
    }
    
    // Check bounds
    size_t headers_len = sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct udphdr);
    if (packetLen < headers_len) {
        return false;
    }
    
    const struct udphdr* udp = (const struct udphdr*)(packetData + sizeof(struct ethhdr) + ip_hdr_len);
    
    // Calculate payload
    payloadData = packetData + headers_len;
    payloadLen = packetLen - headers_len;
    
    // Verify UDP length
    size_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(struct udphdr) || udp_len > payloadLen + sizeof(struct udphdr)) {
        return false;
    }
    
    payloadLen = udp_len - sizeof(struct udphdr);
    
    return true;
}

bool PacketMultiplexer::sendToDestination(const Destination& destination, const uint8_t* data, size_t length) {
    // Use existing RX sockets for TX (following ena-xdp approach)
    // Pick the first available socket (round-robin could be implemented later)
    if (!xdp_sockets_.empty() && xdp_sockets_[0]) {
        try {
            return sendToDestinationZeroCopy(destination, data, length);
        } catch (const std::exception& e) {
            std::cerr << "Zero-copy send failed: " << e.what() << ", falling back to regular socket" << std::endl;
            return sendToDestinationFallback(destination, data, length);
        }
    }
    
    // Fall back to regular socket
    return sendToDestinationFallback(destination, data, length);
}

bool PacketMultiplexer::sendToDestinationZeroCopy(const Destination& destination, const uint8_t* data, size_t length) {
    // Use the first RX socket for TX (ena-xdp approach - same socket for RX/TX)
    if (xdp_sockets_.empty() || !xdp_sockets_[0]) {
        throw std::runtime_error("No XDP sockets available");
    }
    
    AFXDPSocket* xdp_socket = xdp_sockets_[0].get();
    
    // Get a free TX frame from the UMEM
    int tx_frame_offset = xdp_socket->getNextTxFrame();
    if (tx_frame_offset < 0) {
        throw std::runtime_error("No free TX frames available");
    }
    
    // Get pointer to the TX buffer
    uint8_t* tx_buffer = xdp_socket->getUmemBuffer() + tx_frame_offset;
    
    // Create a complete UDP packet with Ethernet, IP, and UDP headers
    size_t packet_len = createUdpPacket(destination, data, length, tx_buffer, 4096);
    if (packet_len == 0) {
        throw std::runtime_error("Failed to create UDP packet");
    }
    
    // Send the packet using AF_XDP zero-copy
    int sent = xdp_socket->send(tx_frame_offset, packet_len);
    if (sent < 0) {
        throw std::runtime_error("AF_XDP send failed");
    }
    
    return sent > 0;
}

bool PacketMultiplexer::sendToDestinationFallback(const Destination& destination, const uint8_t* data, size_t length) {
    // Fallback to regular UDP socket when zero-copy fails
    ssize_t sent = sendto(output_socket_, data, length, 0, 
                         (const struct sockaddr*)&destination.addr, sizeof(destination.addr));
    
    if (sent < 0) {
        std::cerr << "Failed to send to " << destination.ip_address << ":" << destination.port 
                  << " - " << strerror(errno) << std::endl;
        return false;
    }
    
    return sent == static_cast<ssize_t>(length);
}

bool PacketMultiplexer::sendToDestinationWithQueue(const Destination& destination, const uint8_t* data, size_t length, int queueId) {
    // Use the specific queue's socket to avoid race conditions between threads
    if (queueId < 0 || queueId >= num_queues_ || !xdp_sockets_[queueId]) {
        return sendToDestinationFallback(destination, data, length);
    }
    
    try {
        return sendSinglePacketDirect(destination, data, length, queueId);
    } catch (const std::exception& e) {
        std::cerr << "Direct AF_XDP send failed on queue " << queueId << ": " << e.what() << ", falling back to regular socket" << std::endl;
        return sendToDestinationFallback(destination, data, length);
    }
}

bool PacketMultiplexer::sendSinglePacketDirect(const Destination& destination, const uint8_t* data, size_t length, int queueId) {
    // Direct single packet transmission following ena-xdp exactly (no batching)
    AFXDPSocket* xdp_socket = xdp_sockets_[queueId].get();
    
    std::cout << "DEBUG TX: Starting TX for " << destination.ip_address << ":" << destination.port 
              << ", data_len=" << length << ", queue=" << queueId << std::endl;
    
    // Poll completions first (ena-xdp pattern)
    xdp_socket->pollTxCompletions();
    
    // Get next TX frame number (ena-xdp approach)
    int tx_frame_number = xdp_socket->getNextTxFrame();
    uint64_t tx_frame_addr = tx_frame_number * 4096;  // frame_nb * FRAME_SIZE
    
    std::cout << "DEBUG TX: tx_frame_number=" << tx_frame_number << ", tx_frame_addr=0x" 
              << std::hex << tx_frame_addr << std::dec << std::endl;
    
    // Get pointer to TX buffer and create packet
    uint8_t* tx_buffer = xdp_socket->getUmemBuffer() + tx_frame_addr;
    size_t packet_len = createUdpPacket(destination, data, length, tx_buffer, 4096);
    if (packet_len == 0) {
        std::cout << "DEBUG TX: createUdpPacket failed!" << std::endl;
        return false;
    }
    
    std::cout << "DEBUG TX: Created packet, len=" << packet_len << std::endl;
    
    // Print first 64 bytes of packet for debugging
    std::cout << "DEBUG TX: Packet contents (first 64 bytes): ";
    for (size_t i = 0; i < std::min(packet_len, size_t(64)); i++) {
        printf("%02x ", tx_buffer[i]);
        if ((i + 1) % 16 == 0) printf("\n                                     ");
    }
    std::cout << std::endl;
    
    // Direct TX ring operations (following ena-xdp socket_udp_iteration exactly)
    uint32_t tx_idx = 0;
    int ret = xdp_socket->reserveTxRing(1, &tx_idx);  // Reserve 1 descriptor
    if (ret != 1) {
        std::cout << "DEBUG TX: Failed to reserve TX ring, ret=" << ret << std::endl;
        if (ret == 0) {
            xdp_socket->requestDriverPoll();  // Try to wake up
        }
        return false;  // Can't reserve space
    }
    
    std::cout << "DEBUG TX: Reserved TX ring, tx_idx=" << tx_idx << std::endl;
    
    // Fill TX descriptor (ena-xdp pattern)
    xdp_socket->setTxDescriptor(tx_idx, tx_frame_addr, packet_len);
    
    std::cout << "DEBUG TX: Set TX descriptor, addr=0x" << std::hex << tx_frame_addr 
              << std::dec << ", len=" << packet_len << std::endl;
    
    // Submit and wake driver (ena-xdp pattern)
    xdp_socket->submitTxRing(1);
    xdp_socket->requestDriverPoll();
    
    std::cout << "DEBUG TX: Submitted to TX ring and requested driver poll" << std::endl;
    
    return true;
}

size_t PacketMultiplexer::createUdpPacket(const Destination& destination, const uint8_t* payload, size_t payloadLen,
                                         uint8_t* buffer, size_t bufferSize) {
    // Calculate required packet size
    size_t eth_hdr_len = sizeof(struct ethhdr);
    size_t ip_hdr_len = sizeof(struct iphdr);
    size_t udp_hdr_len = sizeof(struct udphdr);
    size_t total_len = eth_hdr_len + ip_hdr_len + udp_hdr_len + payloadLen;
    
    std::cout << "DEBUG createUdpPacket: Creating packet for " << destination.ip_address << ":" << destination.port 
              << ", payload_len=" << payloadLen << ", total_len=" << total_len << std::endl;
    
    if (total_len > bufferSize) {
        std::cerr << "Packet too large for buffer: " << total_len << " > " << bufferSize << std::endl;
        return 0;
    }
    
    // Zero the buffer
    memset(buffer, 0, total_len);
    
    // Create Ethernet header
    struct ethhdr* eth = (struct ethhdr*)buffer;
    // CRITICAL FIX: Use proper destination MAC (following ena-xdp exactly)
    uint8_t dst_mac[ETH_ALEN];
    if (getDestinationMac(destination.ip_address, dst_mac)) {
        memcpy(eth->h_dest, dst_mac, ETH_ALEN);
        std::cout << "DEBUG: Using destination MAC: " << std::hex;
        for (int i = 0; i < ETH_ALEN; i++) {
            std::cout << (int)dst_mac[i] << ":";
        }
        std::cout << std::dec << std::endl;
    } else {
        // Fallback to broadcast MAC if ARP resolution fails
        memset(eth->h_dest, 0xFF, ETH_ALEN);
        std::cout << "DEBUG: Using broadcast MAC (ARP resolution failed)" << std::endl;
    }
    
    // Try to get interface MAC address
    uint8_t src_mac[ETH_ALEN];
    if (getInterfaceMac(listen_interface_, src_mac)) {
        memcpy(eth->h_source, src_mac, ETH_ALEN);
        std::cout << "DEBUG: Using interface MAC: " << std::hex;
        for (int i = 0; i < ETH_ALEN; i++) {
            std::cout << (int)src_mac[i] << ":";
        }
        std::cout << std::dec << std::endl;
    } else {
        // Fallback to a default MAC
        uint8_t default_mac[ETH_ALEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        memcpy(eth->h_source, default_mac, ETH_ALEN);
        std::cout << "DEBUG: Using default MAC (interface MAC not found)" << std::endl;
    }
    eth->h_proto = htons(ETH_P_IP);
    
    // Create IP header
    struct iphdr* ip = (struct iphdr*)(buffer + eth_hdr_len);
    ip->version = 4;
    ip->ihl = 5;  // 20 bytes
    ip->tos = 0;
    ip->tot_len = htons(ip_hdr_len + udp_hdr_len + payloadLen);
    ip->id = htons(12345);  // Simple ID for demo
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->check = 0;  // Will calculate later
    
    // CRITICAL FIX: Use proper source IP - this is likely the main issue!
    std::string interface_ip;
    if (getInterfaceIp(listen_interface_, interface_ip)) {
        inet_aton(interface_ip.c_str(), (struct in_addr*)&ip->saddr);
        std::cout << "DEBUG: Using interface IP as source: " << interface_ip << std::endl;
    } else {
        // Fallback to listen IP
        inet_aton(listen_ip_.c_str(), (struct in_addr*)&ip->saddr);
        std::cout << "DEBUG: Using listen IP as source: " << listen_ip_ << std::endl;
    }
    ip->daddr = destination.addr.sin_addr.s_addr;
    
    // Debug IP addresses
    struct in_addr src_addr, dst_addr;
    src_addr.s_addr = ip->saddr;
    dst_addr.s_addr = ip->daddr;
    std::cout << "DEBUG: IP packet: " << inet_ntoa(src_addr) << " -> " << inet_ntoa(dst_addr) << std::endl;
    
    // Calculate IP checksum (following ena-xdp approach)
    ip->check = 0;  // Reset checksum field
    uint32_t sum = 0;
    uint16_t* ip_words = (uint16_t*)ip;
    for (int i = 0; i < 10; i++) {  // 20 bytes / 2 = 10 words
        sum += ip_words[i];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    ip->check = ~sum;
    
    // Create UDP header
    struct udphdr* udp = (struct udphdr*)(buffer + eth_hdr_len + ip_hdr_len);
    udp->source = htons(listen_port_);  // Use our listen port as source
    udp->dest = destination.addr.sin_port;
    udp->len = htons(udp_hdr_len + payloadLen);
    udp->check = 0;  // Optional for IPv4
    
    std::cout << "DEBUG: UDP packet: port " << listen_port_ << " -> " << destination.port << std::endl;
    
    // Copy payload
    memcpy(buffer + eth_hdr_len + ip_hdr_len + udp_hdr_len, payload, payloadLen);
    
    std::cout << "DEBUG createUdpPacket: Packet created successfully, total length=" << total_len << std::endl;
    
    return total_len;
}

void PacketMultiplexer::initializeOutputSocket() {
    // Create AF_XDP socket for zero-copy transmission (following ena-xdp approach)
    output_xdp_socket_ = std::make_unique<AFXDPSocket>(4096, AFXDPSocket::DEFAULT_UMEM_FRAMES, 0);
    output_xdp_socket_->setupUMem();
    
    // Bind to the same interface we're listening on, but for TX
    // Use zero-copy mode for maximum performance
    output_xdp_socket_->bind(listen_interface_, 0, AFXDPSocket::XDP_FLAGS_ZERO_COPY);
    
    // Pre-populate TX frames with packet templates for better performance
    prePopulateTxFrames();
    
    std::cout << "Created zero-copy AF_XDP output socket on " << listen_interface_ << std::endl;
}

void PacketMultiplexer::prePopulateTxFrames() {
    if (!output_xdp_socket_) {
        return;
    }
    
    uint8_t* umem_buffer = output_xdp_socket_->getUmemBuffer();
    
    // Create packet templates in all TX frames (following ena-xdp approach)
    for (int i = 0; i < AFXDPSocket::DEFAULT_TX_FRAMES; i++) {
        uint8_t* frame_buffer = umem_buffer + (i * 4096);  // Assuming 4096 byte frames
        // Create base packet template that can be modified per destination
        createBasePacketTemplate(frame_buffer);
    }
    
    std::cout << "Pre-populated " << AFXDPSocket::DEFAULT_TX_FRAMES << " TX frames with packet templates" << std::endl;
}

size_t PacketMultiplexer::createBasePacketTemplate(uint8_t* buffer) {
    // Calculate base packet size (headers only, no payload yet)
    size_t eth_hdr_len = sizeof(struct ethhdr);
    size_t ip_hdr_len = sizeof(struct iphdr);
    size_t udp_hdr_len = sizeof(struct udphdr);
    size_t base_len = eth_hdr_len + ip_hdr_len + udp_hdr_len;
    
    // Zero the buffer
    memset(buffer, 0, base_len);
    
    // Create Ethernet header template
    struct ethhdr* eth = (struct ethhdr*)buffer;
    memset(eth->h_dest, 0xFF, ETH_ALEN);   // Will be updated per destination
    memset(eth->h_source, 0x00, ETH_ALEN); // Will be updated with interface MAC
    eth->h_proto = htons(ETH_P_IP);
    
    // Create IP header template
    struct iphdr* ip = (struct iphdr*)(buffer + eth_hdr_len);
    ip->version = 4;
    ip->ihl = 5;  // 20 bytes
    ip->tos = 0;
    ip->tot_len = 0;  // Will be updated per packet
    ip->id = 0;       // Will be updated per packet
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->check = 0;    // Will be calculated per packet
    ip->saddr = 0;    // Will be updated with interface IP
    ip->daddr = 0;    // Will be updated per destination
    
    // Create UDP header template
    struct udphdr* udp = (struct udphdr*)(buffer + eth_hdr_len + ip_hdr_len);
    udp->source = htons(listen_port_);  // Use our listen port as source
    udp->dest = 0;    // Will be updated per destination
    udp->len = 0;     // Will be updated per packet
    udp->check = 0;   // Optional for IPv4
    
    return base_len;
}

size_t PacketMultiplexer::updatePacketForDestination(uint8_t* buffer, const Destination& destination, 
                                                    const uint8_t* payload, size_t payloadLen) {
    size_t eth_hdr_len = sizeof(struct ethhdr);
    size_t ip_hdr_len = sizeof(struct iphdr);
    size_t udp_hdr_len = sizeof(struct udphdr);
    size_t total_len = eth_hdr_len + ip_hdr_len + udp_hdr_len + payloadLen;
    
    // Update IP header
    struct iphdr* ip = (struct iphdr*)(buffer + eth_hdr_len);
    ip->tot_len = htons(ip_hdr_len + udp_hdr_len + payloadLen);
    ip->id = htons(rand() % 65536);  // Random ID
    inet_aton("127.0.0.1", (struct in_addr*)&ip->saddr);  // Demo source IP
    ip->daddr = destination.addr.sin_addr.s_addr;
    
    // Calculate IP checksum (following ena-xdp approach)
    ip->check = 0;  // Reset checksum
    uint32_t sum = 0;
    uint16_t* ip_words = (uint16_t*)ip;
    for (int i = 0; i < 10; i++) {  // 20 bytes / 2 = 10 words
        sum += ip_words[i];  // FIXED: Don't convert to host order
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    ip->check = ~sum;  // FIXED: Don't convert back to network order
    
    // Update UDP header
    struct udphdr* udp = (struct udphdr*)(buffer + eth_hdr_len + ip_hdr_len);
    udp->dest = destination.addr.sin_port;
    udp->len = htons(udp_hdr_len + payloadLen);
    
    // Copy payload
    if (payloadLen > 0) {
        memcpy(buffer + eth_hdr_len + ip_hdr_len + udp_hdr_len, payload, payloadLen);
    }
    
    return total_len;
}

size_t PacketMultiplexer::calculatePacketSize(size_t payloadLen) const {
    return sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + payloadLen;
}

std::vector<uint8_t> PacketMultiplexer::processControlMessage(const uint8_t* message, size_t messageLen, 
                                                             const struct sockaddr_in& clientAddr) {
    if (messageLen < 1) {
        return {}; // Invalid message
    }
    
    uint8_t command = message[0];
    std::vector<uint8_t> response;
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    switch (command) {
        case CTRL_ADD_DESTINATION: {
            if (messageLen >= 7) { // command + 4 bytes IP + 2 bytes port
                uint32_t ip_addr;
                uint16_t port;
                memcpy(&ip_addr, message + 1, 4);
                memcpy(&port, message + 5, 2);
                
                std::string ip_str = formatIpAddress(ip_addr);
                uint16_t port_host = ntohs(port);
                
                std::cout << "Control: ADD_DESTINATION " << ip_str << ":" << port_host 
                          << " from " << client_ip << std::endl;
                
                try {
                    addDestination(ip_str, port_host);
                    response.push_back(1); // Success
                } catch (const std::exception& e) {
                    std::cerr << "Failed to add destination: " << e.what() << std::endl;
                    response.push_back(0); // Failure
                }
            }
            break;
        }
        
        case CTRL_REMOVE_DESTINATION: {
            if (messageLen >= 7) { // command + 4 bytes IP + 2 bytes port
                uint32_t ip_addr;
                uint16_t port;
                memcpy(&ip_addr, message + 1, 4);
                memcpy(&port, message + 5, 2);
                
                std::string ip_str = formatIpAddress(ip_addr);
                uint16_t port_host = ntohs(port);
                
                std::cout << "Control: REMOVE_DESTINATION " << ip_str << ":" << port_host 
                          << " from " << client_ip << std::endl;
                
                try {
                    removeDestination(ip_str, port_host);
                    response.push_back(1); // Success
                } catch (const std::exception& e) {
                    std::cerr << "Failed to remove destination: " << e.what() << std::endl;
                    response.push_back(0); // Failure
                }
            }
            break;
        }
        
        case CTRL_LIST_DESTINATIONS: {
            std::cout << "Control: LIST_DESTINATIONS from " << client_ip << std::endl;
            auto destinations = getDestinations();
            
            response.push_back(static_cast<uint8_t>(destinations.size()));
            for (const auto& dest : destinations) {
                uint32_t ip_addr = parseIpAddress(dest.ip_address);
                uint16_t port_net = htons(dest.port);
                
                response.insert(response.end(), (uint8_t*)&ip_addr, (uint8_t*)&ip_addr + 4);
                response.insert(response.end(), (uint8_t*)&port_net, (uint8_t*)&port_net + 2);
            }
            break;
        }
        
        default:
            std::cout << "Control: Unknown command " << static_cast<int>(command) 
                      << " from " << client_ip << std::endl;
            break;
    }
    
    return response;
}

uint32_t PacketMultiplexer::parseIpAddress(const std::string& ipStr) {
    struct in_addr addr;
    if (inet_aton(ipStr.c_str(), &addr) == 0) {
        throw std::invalid_argument("Invalid IP address: " + ipStr);
    }
    return addr.s_addr; // Already in network byte order
}

std::string PacketMultiplexer::formatIpAddress(uint32_t ipAddr) {
    struct in_addr addr;
    addr.s_addr = ipAddr;
    return std::string(inet_ntoa(addr));
}

bool PacketMultiplexer::getInterfaceIp(const std::string& interface, std::string& ip_address) {
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
    ip_address = inet_ntoa(addr->sin_addr);
    return true;
}

bool PacketMultiplexer::getInterfaceMac(const std::string& interface, uint8_t* mac_address) {
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

bool PacketMultiplexer::getDestinationMac(const std::string& ip_address, uint8_t* mac_address) {
    // Simple ARP table lookup - read /proc/net/arp
    std::ifstream arp_file("/proc/net/arp");
    if (!arp_file.is_open()) {
        return false;
    }
    
    std::string line;
    // Skip header line
    std::getline(arp_file, line);
    
    while (std::getline(arp_file, line)) {
        std::istringstream iss(line);
        std::string ip, hw_type, flags, mac, mask, device;
        
        if (iss >> ip >> hw_type >> flags >> mac >> mask >> device) {
            if (ip == ip_address && mac != "00:00:00:00:00:00") {
                // Parse MAC address
                int values[6];
                if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x",
                          &values[0], &values[1], &values[2],
                          &values[3], &values[4], &values[5]) == 6) {
                    
                    for (int i = 0; i < 6; i++) {
                        mac_address[i] = (uint8_t)values[i];
                    }
                    return true;
                }
            }
        }
    }
    
    return false;
}

void PacketMultiplexer::triggerArpResolution(const std::string& ip_address) {
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
        std::cout << "Sent ARP trigger packet to " << ip_address << std::endl;
        
        // Give some time for ARP resolution to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Check if MAC address is now available
        uint8_t mac[6];
        if (getDestinationMac(ip_address, mac)) {
            std::cout << "ARP resolution successful for " << ip_address << ", MAC: " << std::hex;
            for (int i = 0; i < 6; i++) {
                std::cout << (int)mac[i] << ":";
            }
            std::cout << std::dec << std::endl;
        } else {
            std::cout << "ARP resolution may still be in progress for " << ip_address << std::endl;
        }
    }
    
    close(temp_socket);
}
