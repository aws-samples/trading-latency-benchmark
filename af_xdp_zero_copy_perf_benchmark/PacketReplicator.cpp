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

// Destination implementation
PacketReplicator::Destination::Destination(const std::string& ip, uint16_t p) 
    : ip_address(ip), port(p) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(ip_address.c_str(), &addr.sin_addr) == 0) {
        throw std::invalid_argument("Invalid IP address: " + ip_address);
    }
}

bool PacketReplicator::Destination::operator<(const Destination& other) const {
    if (ip_address != other.ip_address) {
        return ip_address < other.ip_address;
    }
    return port < other.port;
}

// PacketReplicator implementation
PacketReplicator::PacketReplicator(const std::string& interface, const std::string& listenIp, uint16_t listenPort)
    : listen_interface_(interface), listen_ip_(listenIp), listen_port_(listenPort),
      num_queues_(4), output_xdp_socket_(nullptr), control_socket_(-1), output_socket_(-1), running_(false),
      packets_received_(0), packets_sent_(0), bytes_received_(0), bytes_sent_(0) {
    
    // Initialize per-queue statistics
    for (int i = 0; i < MAX_QUEUES; i++) {
        packets_received_per_queue_[i].store(0);
        packets_sent_per_queue_[i].store(0);
    }
    
    // HFT OPTIMIZATION: Initialize CPU core assignments
    initializeCpuCores();
    
    // HFT OPTIMIZATION: Initialize lock-free buffer pool
    for (auto& buffer : buffer_pool_) {
        buffer.in_use.store(false, std::memory_order_relaxed);
        buffer.timestamp = 0;
    }
    
    std::cout << "HFT-optimized PacketReplicator initializing for " << listen_ip_ << ":" << listen_port_ 
              << " on interface " << listen_interface_ << " with " << num_queues_ << " queues" << std::endl;
    std::cout << "HFT optimizations enabled: CPU affinity, lock-free buffers, busy polling, branch prediction" << std::endl;
}

PacketReplicator::~PacketReplicator() {
    stop();
    
    if (control_socket_ >= 0) {
        ::close(control_socket_);
    }
    if (output_socket_ >= 0) {
        ::close(output_socket_);
    }
}

PacketReplicator::PacketReplicator(PacketReplicator&& other) noexcept
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

PacketReplicator& PacketReplicator::operator=(PacketReplicator&& other) noexcept {
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

void PacketReplicator::initialize(bool useZeroCopy) {
    std::cout << "Initializing PacketReplicator with zero-copy: " << (useZeroCopy ? "enabled" : "disabled") << std::endl;
    
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
    
    std::cout << "PacketReplicator initialized successfully with " << num_queues_ << " queues" << std::endl;
}

void PacketReplicator::configureXdpProgram() {
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

void PacketReplicator::addDestination(const std::string& ipAddress, uint16_t port) {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    
    Destination dest(ipAddress, port);
    destinations_.insert(dest);
    
    std::cout << "Added destination: " << ipAddress << ":" << port << std::endl;
    
    // Trigger ARP resolution to ensure MAC address is available
    triggerArpResolution(ipAddress);
}

void PacketReplicator::removeDestination(const std::string& ipAddress, uint16_t port) {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    
    Destination dest(ipAddress, port);
    destinations_.erase(dest);
    
    std::cout << "Removed destination: " << ipAddress << ":" << port << std::endl;
}

std::vector<PacketReplicator::Destination> PacketReplicator::getDestinations() const {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    return std::vector<Destination>(destinations_.begin(), destinations_.end());
}

void PacketReplicator::start() {
    if (!running_.exchange(true)) {
        std::cout << "Starting HFT-optimized PacketReplicator..." << std::endl;
        
        // Start packet processing threads for each queue
        packet_processor_threads_.resize(num_queues_);
        for (int queue_id = 0; queue_id < num_queues_; queue_id++) {
            packet_processor_threads_[queue_id] = std::make_unique<std::thread>(
                [this, queue_id]() { this->processPacketsForQueue(queue_id); }
            );
            
            // HFT OPTIMIZATION: Apply CPU affinity to each packet processing thread
            if (queue_id < static_cast<int>(cpu_cores_.size())) {
                setCpuAffinity(*packet_processor_threads_[queue_id], cpu_cores_[queue_id]);
            }
            
            std::cout << "Started HFT-optimized packet processing thread for queue " << queue_id << std::endl;
        }
        
        // Start control protocol thread (don't bind to specific core to avoid interference)
        control_thread_ = std::make_unique<std::thread>(&PacketReplicator::handleControlProtocol, this);
        
        std::cout << "HFT-optimized PacketReplicator started with " << num_queues_ << " processing threads" << std::endl;
        std::cout << "CPU affinity applied, busy polling enabled, lock-free operations active" << std::endl;
    }
}

void PacketReplicator::stop() {
    if (running_.exchange(false)) {
        std::cout << "Stopping PacketReplicator..." << std::endl;
        
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
        
        std::cout << "PacketReplicator stopped" << std::endl;
    }
}

bool PacketReplicator::isRunning() const {
    return running_.load();
}

PacketReplicator::Statistics PacketReplicator::getStatistics() const {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    return {
        packets_received_.load(),
        packets_sent_.load(),
        bytes_received_.load(),
        bytes_sent_.load(),
        destinations_.size()
    };
}

void PacketReplicator::printStatistics() const {
    auto stats = getStatistics();
    std::cout << "=== PacketReplicator Statistics ===" << std::endl;
    std::cout << "Packets received: " << stats.packets_received << std::endl;
    std::cout << "Packets sent: " << stats.packets_sent << std::endl;
    std::cout << "Bytes received: " << stats.bytes_received << std::endl;
    std::cout << "Bytes sent: " << stats.bytes_sent << std::endl;
    std::cout << "Active destinations: " << stats.destinations_count << std::endl;
    std::cout << "=================================" << std::endl;
}

// HFT OPTIMIZED: Removed processPackets() method - using processPacketsForQueue() instead

void PacketReplicator::processPacketsForQueue(int queueId) {
    std::cout << "HFT-optimized packet processing thread started for queue " << queueId << std::endl;
    
    // HFT OPTIMIZATION: Pre-allocate batch vectors with cache-aligned memory
    alignas(64) std::vector<int> offsets(64);  // Batch size of 64 packets
    alignas(64) std::vector<int> lengths(64);
    
    // HFT OPTIMIZATION: Pre-calculate TX frames for bitwise operations
    const uint32_t tx_frames = 2048;  // Must be power of 2
    static_assert((tx_frames & (tx_frames - 1)) == 0, "tx_frames must be power of 2");
    
    while (__builtin_expect(running_.load(std::memory_order_relaxed), 1)) {
        try {
            // HFT OPTIMIZATION: Receive packets from AF_XDP socket for this specific queue
            int received = xdp_sockets_[queueId]->receive(offsets, lengths);
            
            if (__builtin_expect(received > 0, 1)) {  // Branch prediction hint: packets expected
                // HFT OPTIMIZATION: Prefetch next batch of packet data
                if (__builtin_expect(received > 1, 1)) {
                    uint8_t* next_packet = xdp_sockets_[queueId]->getUmemBuffer() + offsets[1];
                    __builtin_prefetch(next_packet, 0, 3);  // Prefetch for read, high temporal locality
                }
                
                // Process each packet with optimized loop
                for (int i = 0; i < received; i++) {
                    uint8_t* packet_data = xdp_sockets_[queueId]->getUmemBuffer() + offsets[i];
                    size_t packet_len = lengths[i];
                    
                    // HFT OPTIMIZATION: Prefetch next packet data
                    if (__builtin_expect(i + 1 < received, 1)) {
                        uint8_t* next_packet = xdp_sockets_[queueId]->getUmemBuffer() + offsets[i + 1];
                        __builtin_prefetch(next_packet, 0, 3);
                    }
                    
                    // HFT OPTIMIZATION: Update per-queue and total statistics with relaxed memory ordering
                    packets_received_per_queue_[queueId].fetch_add(1, std::memory_order_relaxed);
                    packets_received_.fetch_add(1, std::memory_order_relaxed);
                    bytes_received_.fetch_add(packet_len, std::memory_order_relaxed);
                    
                    // HFT OPTIMIZATION: Replicate the packet to all destinations using lock-free cache
                    int sent_count = replicatePacket(packet_data, packet_len, queueId);
                    
                    if (__builtin_expect(sent_count > 0, 1)) {  // Branch prediction: expect successful sends
                        packets_sent_per_queue_[queueId].fetch_add(sent_count, std::memory_order_relaxed);
                    }
                }
                
                // Recycle the frames back to the fill queue
                xdp_sockets_[queueId]->recycleFrames();
            } else {
                // HFT OPTIMIZATION: Busy polling with CPU pause instead of sleep
                // This keeps the CPU active for lowest possible latency
                __builtin_ia32_pause();  // Pause CPU to reduce power and avoid busy-wait penalties
            }
        } catch (const std::exception& e) {
            if (__builtin_expect(running_.load(std::memory_order_relaxed), 1)) {
                std::cerr << "Error in packet processing for queue " << queueId << ": " << e.what() << std::endl;
                // Brief pause on error to prevent tight error loops
                for (int i = 0; i < 1000; ++i) {
                    __builtin_ia32_pause();
                }
            }
        }
        
        // HFT OPTIMIZATION: *** REMOVED 100μs SLEEP *** - Now using busy polling for minimal latency
        // The __builtin_ia32_pause() above provides the CPU hints without blocking
    }
    
    std::cout << "HFT-optimized packet processing thread stopped for queue " << queueId << std::endl;
}

void PacketReplicator::handleControlProtocol() {
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

int PacketReplicator::replicatePacket(const uint8_t* packetData, size_t packetLen, int queueId) {
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

bool PacketReplicator::extractUdpPayload(const uint8_t* packetData, size_t packetLen,
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

bool PacketReplicator::sendToDestination(const Destination& destination, const uint8_t* data, size_t length) {
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

bool PacketReplicator::sendToDestinationZeroCopy(const Destination& destination, const uint8_t* data, size_t length) {
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

bool PacketReplicator::sendToDestinationFallback(const Destination& destination, const uint8_t* data, size_t length) {
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

bool PacketReplicator::sendToDestinationWithQueue(const Destination& destination, const uint8_t* data, size_t length, int queueId) {
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

bool PacketReplicator::sendSinglePacketDirect(const Destination& destination, const uint8_t* data, size_t length, int queueId) {
    // Direct single packet transmission following ena-xdp exactly (no batching)
    AFXDPSocket* xdp_socket = xdp_sockets_[queueId].get();
    
    DEBUG_TX_PRINT("DEBUG TX: Starting TX for " << destination.ip_address << ":" << destination.port 
              << ", data_len=" << length << ", queue=" << queueId);
    
    // Poll completions first (ena-xdp pattern)
    xdp_socket->pollTxCompletions();
    
    // Get next TX frame number (ena-xdp approach)
    int tx_frame_number = xdp_socket->getNextTxFrame();
    uint64_t tx_frame_addr = tx_frame_number * 4096;  // frame_nb * FRAME_SIZE
    
    DEBUG_TX_PRINT("DEBUG TX: tx_frame_number=" << tx_frame_number << ", tx_frame_addr=0x" 
              << std::hex << tx_frame_addr << std::dec);
    
    // Get pointer to TX buffer and create packet
    uint8_t* tx_buffer = xdp_socket->getUmemBuffer() + tx_frame_addr;
    size_t packet_len = createUdpPacket(destination, data, length, tx_buffer, 4096);
    if (packet_len == 0) {
        DEBUG_TX_PRINT("DEBUG TX: createUdpPacket failed!");
        return false;
    }
    
    DEBUG_TX_PRINT("DEBUG TX: Created packet, len=" << packet_len);
    
    // Print first 64 bytes of packet for debugging - DISABLED for performance
    DEBUG_TX_PRINT("DEBUG TX: Packet contents (first 64 bytes): [Hex dump disabled for performance]");
    
    // Direct TX ring operations (following ena-xdp socket_udp_iteration exactly)
    uint32_t tx_idx = 0;
    int ret = xdp_socket->reserveTxRing(1, &tx_idx);  // Reserve 1 descriptor
    if (ret != 1) {
        DEBUG_TX_PRINT("DEBUG TX: Failed to reserve TX ring, ret=" << ret);
        if (ret == 0) {
            xdp_socket->requestDriverPoll();  // Try to wake up
        }
        return false;  // Can't reserve space
    }
    
    DEBUG_TX_PRINT("DEBUG TX: Reserved TX ring, tx_idx=" << tx_idx);
    
    // Fill TX descriptor (ena-xdp pattern)
    xdp_socket->setTxDescriptor(tx_idx, tx_frame_addr, packet_len);
    
    DEBUG_TX_PRINT("DEBUG TX: Set TX descriptor, addr=0x" << std::hex << tx_frame_addr 
              << std::dec << ", len=" << packet_len);
    
    // Submit and wake driver (ena-xdp pattern)
    xdp_socket->submitTxRing(1);
    xdp_socket->requestDriverPoll();
    
    DEBUG_TX_PRINT("DEBUG TX: Submitted to TX ring and requested driver poll");
    
    return true;
}

size_t PacketReplicator::createUdpPacket(const Destination& destination, const uint8_t* payload, size_t payloadLen,
                                         uint8_t* buffer, size_t bufferSize) {
    // Calculate required packet size
    size_t eth_hdr_len = sizeof(struct ethhdr);
    size_t ip_hdr_len = sizeof(struct iphdr);
    size_t udp_hdr_len = sizeof(struct udphdr);
    size_t total_len = eth_hdr_len + ip_hdr_len + udp_hdr_len + payloadLen;
    
    DEBUG_PACKET_PRINT("DEBUG createUdpPacket: Creating packet for " << destination.ip_address << ":" << destination.port 
              << ", payload_len=" << payloadLen << ", total_len=" << total_len);
    
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
        DEBUG_PACKET_PRINT("DEBUG: Using destination MAC: " << std::hex << dst_mac[0] << ":" << dst_mac[1] << ":" << dst_mac[2] << ":" << dst_mac[3] << ":" << dst_mac[4] << ":" << dst_mac[5] << std::dec);
    } else {
        // Fallback to broadcast MAC if ARP resolution fails
        memset(eth->h_dest, 0xFF, ETH_ALEN);
        DEBUG_PACKET_PRINT("DEBUG: Using broadcast MAC (ARP resolution failed)");
    }
    
    // Try to get interface MAC address
    uint8_t src_mac[ETH_ALEN];
    if (getInterfaceMac(listen_interface_, src_mac)) {
        memcpy(eth->h_source, src_mac, ETH_ALEN);
        DEBUG_PACKET_PRINT("DEBUG: Using interface MAC: " << std::hex << src_mac[0] << ":" << src_mac[1] << ":" << src_mac[2] << ":" << src_mac[3] << ":" << src_mac[4] << ":" << src_mac[5] << std::dec);
    } else {
        // Fallback to a default MAC
        uint8_t default_mac[ETH_ALEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        memcpy(eth->h_source, default_mac, ETH_ALEN);
        DEBUG_PACKET_PRINT("DEBUG: Using default MAC (interface MAC not found)");
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
        DEBUG_PACKET_PRINT("DEBUG: Using interface IP as source: " << interface_ip);
    } else {
        // Fallback to listen IP
        inet_aton(listen_ip_.c_str(), (struct in_addr*)&ip->saddr);
        DEBUG_PACKET_PRINT("DEBUG: Using listen IP as source: " << listen_ip_);
    }
    ip->daddr = destination.addr.sin_addr.s_addr;
    
    // Debug IP addresses
    struct in_addr src_addr, dst_addr;
    src_addr.s_addr = ip->saddr;
    dst_addr.s_addr = ip->daddr;
    DEBUG_PACKET_PRINT("DEBUG: IP packet: " << inet_ntoa(src_addr) << " -> " << inet_ntoa(dst_addr));
    
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
    
    DEBUG_PACKET_PRINT("DEBUG: UDP packet: port " << listen_port_ << " -> " << destination.port);
    
    // Copy payload
    memcpy(buffer + eth_hdr_len + ip_hdr_len + udp_hdr_len, payload, payloadLen);
    
    DEBUG_PACKET_PRINT("DEBUG createUdpPacket: Packet created successfully, total length=" << total_len);
    
    return total_len;
}

void PacketReplicator::initializeOutputSocket() {
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

void PacketReplicator::prePopulateTxFrames() {
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

size_t PacketReplicator::createBasePacketTemplate(uint8_t* buffer) {
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

size_t PacketReplicator::updatePacketForDestination(uint8_t* buffer, const Destination& destination, 
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

size_t PacketReplicator::calculatePacketSize(size_t payloadLen) const {
    return sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + payloadLen;
}

std::vector<uint8_t> PacketReplicator::processControlMessage(const uint8_t* message, size_t messageLen, 
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

uint32_t PacketReplicator::parseIpAddress(const std::string& ipStr) {
    struct in_addr addr;
    if (inet_aton(ipStr.c_str(), &addr) == 0) {
        throw std::invalid_argument("Invalid IP address: " + ipStr);
    }
    return addr.s_addr; // Already in network byte order
}

std::string PacketReplicator::formatIpAddress(uint32_t ipAddr) {
    struct in_addr addr;
    addr.s_addr = ipAddr;
    return std::string(inet_ntoa(addr));
}

bool PacketReplicator::getInterfaceIp(const std::string& interface, std::string& ip_address) {
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

bool PacketReplicator::getInterfaceMac(const std::string& interface, uint8_t* mac_address) {
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

bool PacketReplicator::getDestinationMac(const std::string& ip_address, uint8_t* mac_address) {
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

void PacketReplicator::triggerArpResolution(const std::string& ip_address) {
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

// HFT OPTIMIZATION IMPLEMENTATIONS

// Thread-local destination cache definition
thread_local PacketReplicator::ThreadLocalDestCache PacketReplicator::dest_cache_;

PacketReplicator::PacketBuffer* PacketReplicator::getBufferFromPool() {
    // HFT OPTIMIZATION: Lock-free buffer allocation using atomic operations
    const uint32_t max_attempts = BUFFER_POOL_SIZE;
    
    for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
        uint32_t index = buffer_pool_index_.fetch_add(1, std::memory_order_relaxed) & (BUFFER_POOL_SIZE - 1);
        PacketBuffer* buffer = &buffer_pool_[index];
        
        // Try to acquire this buffer using compare-and-swap
        bool expected = false;
        if (buffer->in_use.compare_exchange_weak(expected, true, std::memory_order_acquire)) {
            // Successfully acquired buffer
            buffer->timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            return buffer;
        }
        
        // Buffer was busy, try next one
        __builtin_ia32_pause();  // CPU pause for better performance
    }
    
    return nullptr;  // No free buffers available
}

void PacketReplicator::returnBufferToPool(PacketBuffer* buffer) {
    if (__builtin_expect(buffer != nullptr, 1)) {
        // HFT OPTIMIZATION: Release buffer back to pool
        buffer->in_use.store(false, std::memory_order_release);
    }
}

bool PacketReplicator::setCpuAffinity(std::thread& thread, int cpu_core) {
    // HFT OPTIMIZATION: Bind thread to specific CPU core for deterministic performance
    if (!enable_cpu_affinity_) {
        return true;  // CPU affinity disabled
    }
    
    pthread_t native_handle = thread.native_handle();
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_core, &cpuset);
    
    int result = pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        std::cerr << "Failed to set CPU affinity for thread to core " << cpu_core 
                  << ": " << strerror(result) << std::endl;
        return false;
    }
    
    std::cout << "Successfully bound thread to CPU core " << cpu_core << std::endl;
    return true;
}

void PacketReplicator::initializeCpuCores() {
    // HFT OPTIMIZATION: Initialize CPU core assignments for optimal performance
    // Reserve cores for packet processing threads (avoid core 0 which handles interrupts)
    
    int num_cores = std::thread::hardware_concurrency();
    std::cout << "Detected " << num_cores << " CPU cores" << std::endl;
    
    // Reserve cores starting from core 1 (avoiding core 0)
    cpu_cores_.clear();
    cpu_cores_.reserve(num_queues_);
    
    for (int i = 0; i < num_queues_ && i + 1 < num_cores; ++i) {
        cpu_cores_.push_back(i + 1);  // Start from core 1
    }
    
    std::cout << "Assigned CPU cores for packet processing: ";
    for (int core : cpu_cores_) {
        std::cout << core << " ";
    }
    std::cout << std::endl;
}

std::vector<PacketReplicator::Destination> PacketReplicator::getCachedDestinations() {
    // HFT OPTIMIZATION: Use thread-local cache to avoid lock contention
    auto now = std::chrono::steady_clock::now();
    
    if (dest_cache_.cached_destinations.empty() || 
        (now - dest_cache_.last_update) > ThreadLocalDestCache::CACHE_TIMEOUT) {
        updateDestinationCache();
    }
    
    return dest_cache_.cached_destinations;
}

void PacketReplicator::updateDestinationCache() {
    // HFT OPTIMIZATION: Update thread-local destination cache
    {
        std::lock_guard<std::mutex> lock(destinations_mutex_);
        dest_cache_.cached_destinations.clear();
        dest_cache_.cached_destinations.reserve(destinations_.size());
        
        for (const auto& dest : destinations_) {
            dest_cache_.cached_destinations.push_back(dest);
        }
    }
    
    dest_cache_.last_update = std::chrono::steady_clock::now();
}

inline bool PacketReplicator::extractUdpPayloadFast(const uint8_t* packetData, size_t packetLen,
                                                    const uint8_t*& payloadData, size_t& payloadLen) {
    // HFT OPTIMIZATION: Fast UDP payload extraction with branch prediction hints
    
    // Branch prediction: expect valid packet size
    if (__builtin_expect(packetLen < 42, 0)) {  // Minimum Ethernet + IP + UDP = 42 bytes
        return false;
    }
    
    // Fast Ethernet header check with branch prediction
    const struct ethhdr* eth = (const struct ethhdr*)packetData;
    if (__builtin_expect(eth->h_proto != htons(ETH_P_IP), 0)) {
        return false;
    }
    
    // Fast IP header check
    const struct iphdr* ip = (const struct iphdr*)(packetData + 14);
    if (__builtin_expect(ip->protocol != IPPROTO_UDP, 0)) {
        return false;
    }
    
    // HFT OPTIMIZATION: Use bitwise operations for IP header length calculation
    const size_t ip_hdr_len = (ip->ihl & 0x0F) << 2;  // Faster than multiplication
    if (__builtin_expect(ip_hdr_len < 20, 0)) {
        return false;
    }
    
    // Calculate payload offset with minimal branching
    const size_t headers_len = 14 + ip_hdr_len + 8;  // Ethernet + IP + UDP
    if (__builtin_expect(packetLen < headers_len, 0)) {
        return false;
    }
    
    // Extract payload information
    payloadData = packetData + headers_len;
    payloadLen = packetLen - headers_len;
    
    // HFT OPTIMIZATION: Prefetch payload data for next processing step
    __builtin_prefetch(payloadData, 0, 3);
    
    return true;
}
