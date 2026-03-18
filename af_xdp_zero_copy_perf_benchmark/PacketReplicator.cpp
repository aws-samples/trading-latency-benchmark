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
    memset(mac, 0xFF, sizeof(mac));  // Default: broadcast; replaced after ARP resolution
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
      num_queues_(4), multicast_socket_(-1), gre_mode_(false), output_xdp_socket_(nullptr),
      control_socket_(-1), output_socket_(-1),
      ctrl_multicast_port_(0), producer_port_(0),
      ctrl_multicast_socket_(-1), ctrl_forward_socket_(-1),
      running_(false),
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

    if (multicast_socket_ >= 0) {
        ::close(multicast_socket_);
        multicast_socket_ = -1;
    }
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
    
    // Select XDP program:
    //   gre_mode_       → gre_filter.o   (outer unicast GRE carries inner multicast)
    //   multicast IP    → multicast_filter.o (native multicast on NIC, e.g. TGW)
    //   unicast IP      → unicast_filter.o   (direct unicast feed)
    std::string xdp_program_path;
    if (gre_mode_) {
        xdp_program_path = "./gre_filter.o";
    } else if (isMulticastAddress(listen_ip_)) {
        xdp_program_path = "./multicast_filter.o";
    } else {
        xdp_program_path = "./unicast_filter.o";
    }
    std::cout << "Loading XDP program: " << xdp_program_path
              << (gre_mode_ ? " (GRE tunnel mode)" : "") << std::endl;
    AFXDPSocket::loadXdpProgram(listen_interface_, xdp_program_path, useZeroCopy);

    // Configure XDP program with target IP (inner multicast group) and port
    configureXdpProgram();

    // IGMP join only for native multicast — GRE outer packet is unicast so no join needed
    if (!gre_mode_ && isMulticastAddress(listen_ip_)) {
        joinMulticastGroup();
    }
    
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

    // Cache interface IP and MAC once — createUdpPacket() reads these on every TX packet
    if (!getInterfaceIp(listen_interface_, cached_iface_ip_))
        throw std::runtime_error("Failed to get IP for interface " + listen_interface_);
    if (!getInterfaceMac(listen_interface_, cached_iface_mac_))
        throw std::runtime_error("Failed to get MAC for interface " + listen_interface_);
    std::cout << "Interface " << listen_interface_
              << " IP=" << cached_iface_ip_ << std::endl;

    // Upstream control: join control multicast group and prepare forward socket
    if (!ctrl_multicast_group_.empty()) {
        joinControlMulticastGroup();
        ctrl_forward_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (ctrl_forward_socket_ < 0) {
            throw std::runtime_error("Failed to create ctrl forward socket: " + std::string(strerror(errno)));
        }
        std::cout << "Upstream control configured: " << ctrl_multicast_group_ << ":"
                  << ctrl_multicast_port_ << " → " << producer_ip_ << ":" << producer_port_ << std::endl;
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

bool PacketReplicator::isMulticastAddress(const std::string& ip) {
    struct in_addr addr;
    if (inet_aton(ip.c_str(), &addr) == 0)
        return false;
    // First octet 224–239 (0xE0–0xEF) — multicast range 224.0.0.0/4
    uint8_t first_octet = ntohl(addr.s_addr) >> 24;
    return first_octet >= 224 && first_octet <= 239;
}

void PacketReplicator::joinMulticastGroup() {
    multicast_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (multicast_socket_ < 0) {
        throw std::runtime_error("Failed to create multicast socket: " + std::string(strerror(errno)));
    }

    // Bind to the port so the socket is valid for membership tracking
    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(listen_port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(multicast_socket_, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        // Non-fatal: another socket may already be bound; membership join still works
        std::cerr << "Warning: multicast socket bind failed (port " << listen_port_
                  << "): " << strerror(errno) << " — continuing" << std::endl;
    }

    struct ip_mreqn mreq = {};
    if (inet_aton(listen_ip_.c_str(), &mreq.imr_multiaddr) == 0) {
        ::close(multicast_socket_);
        multicast_socket_ = -1;
        throw std::runtime_error("Invalid multicast group address: " + listen_ip_);
    }
    mreq.imr_address.s_addr = INADDR_ANY;
    mreq.imr_ifindex = static_cast<int>(if_nametoindex(listen_interface_.c_str()));
    if (mreq.imr_ifindex == 0) {
        ::close(multicast_socket_);
        multicast_socket_ = -1;
        throw std::runtime_error("Unknown interface: " + listen_interface_);
    }

    if (setsockopt(multicast_socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ::close(multicast_socket_);
        multicast_socket_ = -1;
        throw std::runtime_error("Failed to join multicast group " + listen_ip_ +
                                 ": " + strerror(errno));
    }

    std::cout << "Joined multicast group " << listen_ip_
              << " on interface " << listen_interface_ << std::endl;
}

void PacketReplicator::leaveMulticastGroup() {
    if (multicast_socket_ < 0)
        return;

    struct ip_mreqn mreq = {};
    inet_aton(listen_ip_.c_str(), &mreq.imr_multiaddr);
    mreq.imr_address.s_addr = INADDR_ANY;
    mreq.imr_ifindex = static_cast<int>(if_nametoindex(listen_interface_.c_str()));

    setsockopt(multicast_socket_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    ::close(multicast_socket_);
    multicast_socket_ = -1;
    std::cout << "Left multicast group " << listen_ip_ << std::endl;
}

void PacketReplicator::setUpstreamControl(const std::string& ctrlGroup, uint16_t ctrlPort,
                                          const std::string& producerIp, uint16_t producerPort) {
    ctrl_multicast_group_ = ctrlGroup;
    ctrl_multicast_port_  = ctrlPort;
    producer_ip_          = producerIp;
    producer_port_        = producerPort;
}

void PacketReplicator::joinControlMulticastGroup() {
    ctrl_multicast_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctrl_multicast_socket_ < 0)
        throw std::runtime_error("Failed to create ctrl multicast socket: " + std::string(strerror(errno)));

    int reuse = 1;
    setsockopt(ctrl_multicast_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(ctrl_multicast_port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(ctrl_multicast_socket_, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        std::cerr << "Warning: ctrl multicast socket bind failed: " << strerror(errno) << " — continuing" << std::endl;
    }

    struct ip_mreqn mreq{};
    if (inet_aton(ctrl_multicast_group_.c_str(), &mreq.imr_multiaddr) == 0) {
        ::close(ctrl_multicast_socket_);
        ctrl_multicast_socket_ = -1;
        throw std::runtime_error("Invalid ctrl multicast group: " + ctrl_multicast_group_);
    }
    mreq.imr_address.s_addr = INADDR_ANY;
    mreq.imr_ifindex = static_cast<int>(if_nametoindex(listen_interface_.c_str()));
    if (mreq.imr_ifindex == 0) {
        ::close(ctrl_multicast_socket_);
        ctrl_multicast_socket_ = -1;
        throw std::runtime_error("Unknown interface: " + listen_interface_);
    }
    if (setsockopt(ctrl_multicast_socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ::close(ctrl_multicast_socket_);
        ctrl_multicast_socket_ = -1;
        throw std::runtime_error("Failed to join ctrl multicast group " + ctrl_multicast_group_ +
                                 ": " + strerror(errno));
    }
    std::cout << "Joined ctrl multicast group " << ctrl_multicast_group_
              << " on interface " << listen_interface_ << std::endl;
}

void PacketReplicator::handleUpstreamControl() {
    std::cout << "Upstream control thread started: " << ctrl_multicast_group_ << ":"
              << ctrl_multicast_port_ << " → " << producer_ip_ << ":" << producer_port_ << std::endl;

    struct sockaddr_in producer_addr{};
    producer_addr.sin_family = AF_INET;
    producer_addr.sin_port   = htons(producer_port_);
    inet_aton(producer_ip_.c_str(), &producer_addr.sin_addr);

    // 1-second recv timeout so the loop checks running_ regularly
    struct timeval tv{1, 0};
    setsockopt(ctrl_multicast_socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::vector<uint8_t> buf(65535);
    struct sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);

    while (running_) {
        ssize_t n = recvfrom(ctrl_multicast_socket_, buf.data(), buf.size(), 0,
                             (struct sockaddr*)&sender, &sender_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (running_)
                std::cerr << "Upstream control recv error: " << strerror(errno) << std::endl;
            continue;
        }
        if (n == 0)
            continue;

        ssize_t sent = sendto(ctrl_forward_socket_, buf.data(), static_cast<size_t>(n), 0,
                              (struct sockaddr*)&producer_addr, sizeof(producer_addr));
        if (sent < 0 && running_)
            std::cerr << "Upstream control forward error: " << strerror(errno) << std::endl;
    }

    std::cout << "Upstream control thread stopped" << std::endl;
}

void PacketReplicator::addDestination(const std::string& ipAddress, uint16_t port) {
    // ARP trigger and MAC lookup happen outside the lock: triggerArpResolution sleeps
    // 100ms and getDestinationMac reads /proc/net/arp — both unacceptable inside the
    // mutex that the packet-processing hot path acquires via getCachedDestinations().
    triggerArpResolution(ipAddress);

    Destination dest(ipAddress, port);
    if (!getDestinationMac(ipAddress, dest.mac)) {
        std::cerr << "Warning: ARP not resolved for " << ipAddress
                  << " — using broadcast MAC until next addDestination call" << std::endl;
        // dest.mac already set to 0xFF:FF:FF:FF:FF:FF in constructor
    }

    std::lock_guard<std::mutex> lock(destinations_mutex_);
    destinations_.insert(dest);
    std::cout << "Added destination: " << ipAddress << ":" << port << std::endl;
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

        // Start upstream control forwarding thread if configured
        if (!ctrl_multicast_group_.empty()) {
            ctrl_upstream_thread_ = std::make_unique<std::thread>(&PacketReplicator::handleUpstreamControl, this);
            std::cout << "Started upstream control thread" << std::endl;
        }

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

        // Wait for upstream control thread to finish
        if (ctrl_upstream_thread_ && ctrl_upstream_thread_->joinable()) {
            ctrl_upstream_thread_->join();
            ctrl_upstream_thread_.reset();
        }

        // Leave control multicast group
        if (ctrl_multicast_socket_ >= 0) {
            struct ip_mreqn mreq{};
            inet_aton(ctrl_multicast_group_.c_str(), &mreq.imr_multiaddr);
            mreq.imr_address.s_addr = INADDR_ANY;
            mreq.imr_ifindex = static_cast<int>(if_nametoindex(listen_interface_.c_str()));
            setsockopt(ctrl_multicast_socket_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
            ::close(ctrl_multicast_socket_);
            ctrl_multicast_socket_ = -1;
        }
        if (ctrl_forward_socket_ >= 0) {
            ::close(ctrl_forward_socket_);
            ctrl_forward_socket_ = -1;
        }

        // Drop multicast membership — only for native multicast mode (GRE never joined)
        if (!gre_mode_ && isMulticastAddress(listen_ip_)) {
            leaveMulticastGroup();
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
    
    // Const ref to thread-local cache — no copy, no lock on hot path
    const std::vector<Destination>& current_destinations = getCachedDestinations();
    if (current_destinations.empty()) {
        return 0;
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

bool PacketReplicator::extractUdpPayloadGre(const uint8_t* packetData, size_t packetLen,
                                             const uint8_t*& payloadData, size_t& payloadLen) {
    // Minimum: Eth(14) + outerIP(20) + GRE(4) + innerIP(20) + UDP(8) = 66 bytes
    static constexpr size_t MIN_GRE_PKT = 14 + 20 + 4 + 20 + 8;
    if (packetLen < MIN_GRE_PKT)
        return false;

    const struct ethhdr* eth = reinterpret_cast<const struct ethhdr*>(packetData);
    if (ntohs(eth->h_proto) != ETH_P_IP)
        return false;

    const struct iphdr* outer_ip = reinterpret_cast<const struct iphdr*>(
        packetData + sizeof(struct ethhdr));
    if (outer_ip->protocol != IPPROTO_GRE)
        return false;

    size_t outer_ip_len = outer_ip->ihl * 4;
    if (outer_ip_len < 20 || outer_ip_len > 60)
        return false;

    // GRE fixed header: 2-byte flags + 2-byte protocol
    size_t gre_offset = sizeof(struct ethhdr) + outer_ip_len;
    if (packetLen < gre_offset + 4)
        return false;

    const uint8_t* gre = packetData + gre_offset;
    uint16_t gre_flags = static_cast<uint16_t>((gre[0] << 8) | gre[1]);
    uint16_t gre_proto = static_cast<uint16_t>((gre[2] << 8) | gre[3]);

    if (gre_proto != ETH_P_IP)  // inner must be IPv4
        return false;

    // Variable GRE header size: base 4 bytes + optional fields
    size_t gre_len = 4;
    if (gre_flags & 0x8000) gre_len += 4;  // checksum + reserved
    if (gre_flags & 0x2000) gre_len += 4;  // key
    if (gre_flags & 0x1000) gre_len += 4;  // sequence number

    // Inner IPv4
    size_t inner_ip_offset = gre_offset + gre_len;
    if (packetLen < inner_ip_offset + sizeof(struct iphdr))
        return false;

    const struct iphdr* inner_ip = reinterpret_cast<const struct iphdr*>(
        packetData + inner_ip_offset);
    if (inner_ip->protocol != IPPROTO_UDP)
        return false;

    size_t inner_ip_len = inner_ip->ihl * 4;
    if (inner_ip_len < 20 || inner_ip_len > 60)
        return false;

    // Inner UDP
    size_t udp_offset = inner_ip_offset + inner_ip_len;
    if (packetLen < udp_offset + sizeof(struct udphdr))
        return false;

    const struct udphdr* udp = reinterpret_cast<const struct udphdr*>(
        packetData + udp_offset);

    size_t udp_payload_len = ntohs(udp->len);
    if (udp_payload_len < sizeof(struct udphdr))
        return false;
    udp_payload_len -= sizeof(struct udphdr);

    if (packetLen < udp_offset + sizeof(struct udphdr) + udp_payload_len)
        return false;

    payloadData = packetData + udp_offset + sizeof(struct udphdr);
    payloadLen  = udp_payload_len;
    return true;
}

bool PacketReplicator::extractUdpPayload(const uint8_t* packetData, size_t packetLen,
                                         const uint8_t*& payloadData, size_t& payloadLen) {
    if (gre_mode_)
        return extractUdpPayloadGre(packetData, packetLen, payloadData, payloadLen);

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
    // If ARP has not yet resolved for this destination, the cached MAC is all-broadcast.
    // ENA/VPC drops frames with broadcast dst MAC, so route through the kernel socket
    // which handles ARP internally.  updateDestinationCache() will re-resolve the MAC on
    // the next 100ms cache refresh and automatically restore the AF_XDP fast path.
    static constexpr uint8_t BROADCAST_MAC[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (__builtin_expect(memcmp(destination.mac, BROADCAST_MAC, ETH_ALEN) == 0, 0)) {
        return sendToDestinationFallback(destination, data, length);
    }

    if (queueId < 0 || queueId >= num_queues_ || !xdp_sockets_[queueId]) {
        return sendToDestinationFallback(destination, data, length);
    }

    try {
        return sendSinglePacketDirect(destination, data, length, queueId);
    } catch (const std::exception& e) {
        std::cerr << "Direct AF_XDP send failed on queue " << queueId << ": " << e.what()
                  << ", falling back to regular socket" << std::endl;
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

    // Ethernet header — MACs cached at initialize()/addDestination(); zero syscalls on hot path.
    // Broadcast dst MAC (0xFF * 6) signals unresolved ARP; sendToDestinationWithQueue() routes
    // those packets to sendToDestinationFallback() before createUdpPacket() is ever called.
    struct ethhdr* eth = (struct ethhdr*)buffer;
    memcpy(eth->h_dest,   destination.mac,  ETH_ALEN);
    memcpy(eth->h_source, cached_iface_mac_, ETH_ALEN);
    eth->h_proto = htons(ETH_P_IP);

    // IP header
    struct iphdr* ip = (struct iphdr*)(buffer + eth_hdr_len);
    ip->version  = 4;
    ip->ihl      = 5;
    ip->tos      = 0;
    ip->tot_len  = htons(ip_hdr_len + udp_hdr_len + payloadLen);
    ip->id       = 0;             // Atomic datagram: ID=0 per RFC 6864 when DF is set
    ip->frag_off = htons(IP_DF);  // Don't Fragment — market data always fits within MTU
    ip->ttl      = 64;
    ip->protocol = IPPROTO_UDP;
    ip->check    = 0;
    inet_aton(cached_iface_ip_.c_str(), (struct in_addr*)&ip->saddr);
    ip->daddr    = destination.addr.sin_addr.s_addr;
    
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
        // Poll /proc/net/arp at 1ms intervals instead of a fixed sleep.
        // AWS VPC same-AZ ARP resolves in ~3-5ms; cross-AZ up to ~15ms.
        // Cap at 50ms — if unresolved by then, addDestination() stores broadcast MAC
        // and the self-healing path in updateDestinationCache() retries every 100ms.
        static constexpr int POLL_INTERVAL_MS = 1;
        static constexpr int MAX_WAIT_MS      = 50;
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

const std::vector<PacketReplicator::Destination>& PacketReplicator::getCachedDestinations() {
    auto now = std::chrono::steady_clock::now();
    if (dest_cache_.cached_destinations.empty() ||
        (now - dest_cache_.last_update) > ThreadLocalDestCache::CACHE_TIMEOUT) {
        updateDestinationCache();
    }
    return dest_cache_.cached_destinations;  // const ref — zero copy on hot path
}

void PacketReplicator::updateDestinationCache() {
    {
        std::lock_guard<std::mutex> lock(destinations_mutex_);
        dest_cache_.cached_destinations.clear();
        dest_cache_.cached_destinations.reserve(destinations_.size());
        for (const auto& dest : destinations_) {
            dest_cache_.cached_destinations.push_back(dest);
        }
    }

    // Re-resolve any broadcast MACs in the thread-local copy.  The authoritative
    // std::set keeps the broadcast MAC (immutable keys), but once ARP resolves,
    // the thread-local cache gets the real MAC and the AF_XDP fast path resumes
    // on the next packet — without requiring a removeDestination/addDestination cycle.
    static constexpr uint8_t BROADCAST_MAC[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    for (auto& dest : dest_cache_.cached_destinations) {
        if (memcmp(dest.mac, BROADCAST_MAC, ETH_ALEN) == 0) {
            getDestinationMac(dest.ip_address, dest.mac);  // updates thread-local copy only
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
