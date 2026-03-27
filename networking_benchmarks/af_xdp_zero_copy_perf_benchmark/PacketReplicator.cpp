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
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <errno.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <ctime>

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
      num_queues_(4), gre_mode_(false),
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
    
    // Initialize CPU core assignments
    initializeCpuCores();

    std::cout << "PacketReplicator initializing for " << listen_ip_ << ":" << listen_port_
              << " on interface " << listen_interface_ << " with " << num_queues_ << " queues" << std::endl;
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
      gre_mode_(other.gre_mode_),
      config_map_fd_(other.config_map_fd_),
      group_slots_(std::move(other.group_slots_)),
      group_ref_counts_(std::move(other.group_ref_counts_)),
      free_slots_(std::move(other.free_slots_)),
      group_destinations_(std::move(other.group_destinations_)),
      listen_ip_nbo_(other.listen_ip_nbo_),
      ctrl_multicast_group_(std::move(other.ctrl_multicast_group_)),
      ctrl_multicast_port_(other.ctrl_multicast_port_),
      producer_ip_(std::move(other.producer_ip_)),
      producer_port_(other.producer_port_),
      ctrl_multicast_socket_(other.ctrl_multicast_socket_),
      ctrl_forward_socket_(other.ctrl_forward_socket_),
      cached_iface_ip_(std::move(other.cached_iface_ip_)),
      xdp_sockets_(std::move(other.xdp_sockets_)),
      control_socket_(other.control_socket_),
      output_socket_(other.output_socket_),
      running_(other.running_.load()),
      packet_processor_threads_(std::move(other.packet_processor_threads_)),
      control_thread_(std::move(other.control_thread_)),
      ctrl_upstream_thread_(std::move(other.ctrl_upstream_thread_)),
      all_destinations_(std::move(other.all_destinations_)),
      packets_received_(other.packets_received_.load()),
      packets_sent_(other.packets_sent_.load()),
      bytes_received_(other.bytes_received_.load()),
      bytes_sent_(other.bytes_sent_.load()) {

    memcpy(cached_iface_mac_, other.cached_iface_mac_, sizeof(cached_iface_mac_));

    // Copy per-queue statistics arrays
    for (int i = 0; i < MAX_QUEUES; i++) {
        packets_received_per_queue_[i].store(other.packets_received_per_queue_[i].load());
        packets_sent_per_queue_[i].store(other.packets_sent_per_queue_[i].load());
    }

    other.config_map_fd_ = -1;
    other.ctrl_multicast_socket_ = -1;
    other.ctrl_forward_socket_ = -1;
    other.control_socket_ = -1;
    other.output_socket_ = -1;
    other.running_ = false;
}

PacketReplicator& PacketReplicator::operator=(PacketReplicator&& other) noexcept {
    if (this != &other) {
        stop();

        listen_interface_      = std::move(other.listen_interface_);
        listen_ip_             = std::move(other.listen_ip_);
        listen_port_           = other.listen_port_;
        num_queues_            = other.num_queues_;
        gre_mode_              = other.gre_mode_;
        config_map_fd_         = other.config_map_fd_;
        group_slots_           = std::move(other.group_slots_);
        group_ref_counts_      = std::move(other.group_ref_counts_);
        free_slots_            = std::move(other.free_slots_);
        group_destinations_    = std::move(other.group_destinations_);
        listen_ip_nbo_         = other.listen_ip_nbo_;
        ctrl_multicast_group_  = std::move(other.ctrl_multicast_group_);
        ctrl_multicast_port_   = other.ctrl_multicast_port_;
        producer_ip_           = std::move(other.producer_ip_);
        producer_port_         = other.producer_port_;
        ctrl_multicast_socket_ = other.ctrl_multicast_socket_;
        ctrl_forward_socket_   = other.ctrl_forward_socket_;
        cached_iface_ip_       = std::move(other.cached_iface_ip_);
        memcpy(cached_iface_mac_, other.cached_iface_mac_, sizeof(cached_iface_mac_));
        xdp_sockets_           = std::move(other.xdp_sockets_);
        control_socket_        = other.control_socket_;
        output_socket_         = other.output_socket_;
        running_               = other.running_.load();
        packet_processor_threads_ = std::move(other.packet_processor_threads_);
        control_thread_        = std::move(other.control_thread_);
        ctrl_upstream_thread_  = std::move(other.ctrl_upstream_thread_);
        all_destinations_      = std::move(other.all_destinations_);
        packets_received_      = other.packets_received_.load();
        packets_sent_          = other.packets_sent_.load();
        bytes_received_        = other.bytes_received_.load();
        bytes_sent_            = other.bytes_sent_.load();

        for (int i = 0; i < MAX_QUEUES; i++) {
            packets_received_per_queue_[i].store(other.packets_received_per_queue_[i].load());
            packets_sent_per_queue_[i].store(other.packets_sent_per_queue_[i].load());
        }

        other.config_map_fd_         = -1;
        other.ctrl_multicast_socket_ = -1;
        other.ctrl_forward_socket_   = -1;
        other.control_socket_        = -1;
        other.output_socket_         = -1;
        other.running_               = false;
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
    //   gre_mode_  → gre_filter.o     (outer unicast GRE carries inner multicast)
    //   otherwise  → unicast_filter.o (direct unicast feed)
    std::string xdp_program_path = gre_mode_ ? "./gre_filter.o" : "./unicast_filter.o";
    std::cout << "Loading XDP program: " << xdp_program_path
              << (gre_mode_ ? " (GRE tunnel mode)" : "") << std::endl;
    AFXDPSocket::loadXdpProgram(listen_interface_, xdp_program_path, useZeroCopy);

    // Cache listen_ip_ as NBO for use by configureXdpProgram() and updateDestinationCache()
    listen_ip_nbo_ = parseIpAddress(listen_ip_);

    // Configure XDP program with target IP and port
    configureXdpProgram();

    // In GRE mode seed the inner multicast group immediately into config_map slot 0
    // so the BPF filter starts redirecting frames before any subscriber joins.
    if (gre_mode_) {
        addGroupDynamic(listen_ip_nbo_);
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
    // GRE mode uses a raw socket so the fallback path can send GRE-encapsulated packets.
    // The kernel builds the outer IP header; we provide GRE header + inner IP datagram.
    if (gre_mode_) {
        output_socket_ = socket(AF_INET, SOCK_RAW, IPPROTO_GRE);
    } else {
        output_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    }
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
    static constexpr int MAX_GROUPS = 16;

    config_map_fd_ = AFXDPSocket::getXdpMapFd("config_map");
    if (config_map_fd_ < 0) {
        throw std::runtime_error("Could not find config_map in loaded XDP program — cannot configure filter");
    }

    struct unicast_config {
        uint32_t target_ip;
        uint16_t target_port;
        uint16_t padding;
    };

    // Zero all slots first
    unicast_config zero{};
    for (uint32_t i = 0; i < MAX_GROUPS; ++i)
        bpf_map_update_elem(config_map_fd_, &i, &zero, BPF_ANY);

    // Unicast mode: write listen_ip_/listen_port_ into slot 0 statically.
    // GRE mode: slot 0 (and others) are populated dynamically by addGroupDynamic().
    int first_free_slot = 0;
    if (!gre_mode_) {
        unicast_config cfg{};
        cfg.target_ip   = listen_ip_nbo_;
        cfg.target_port = htons(listen_port_);
        bpf_map_update_elem(config_map_fd_, &first_free_slot, &cfg, BPF_ANY);
        first_free_slot = 1;  // slot 0 is taken; dynamic alloc starts from slot 1
        std::cout << "Unicast filter: seeded slot 0 with " << listen_ip_ << ":" << listen_port_ << std::endl;
    }

    // Initialise the free-slot pool (GRE: all 16 slots; unicast: slots 1–15)
    free_slots_.clear();
    for (int i = MAX_GROUPS - 1; i >= first_free_slot; --i)
        free_slots_.push_back(static_cast<uint32_t>(i));
}

void PacketReplicator::addGroupDynamic(uint32_t group_nbo) {
    const std::string group_str = formatIpAddress(group_nbo);

    std::lock_guard<std::mutex> lock(group_mutex_);

    // Already tracking — bump reference count
    auto ref_it = group_ref_counts_.find(group_nbo);
    if (ref_it != group_ref_counts_.end()) {
        ++ref_it->second;
        return;
    }

    // Grab a free config_map slot
    if (free_slots_.empty()) {
        std::cerr << "[GRE] config_map full (max 16 groups); ignoring Join for "
                  << group_str << std::endl;
        return;
    }
    uint32_t slot = free_slots_.back();
    free_slots_.pop_back();

    // Write group entry into the BPF filter map
    struct { uint32_t target_ip; uint16_t target_port; uint16_t padding; } cfg{};
    cfg.target_ip   = group_nbo;
    cfg.target_port = htons(listen_port_);
    if (bpf_map_update_elem(config_map_fd_, &slot, &cfg, BPF_ANY) != 0) {
        std::cerr << "[GRE] bpf_map_update_elem failed for " << group_str
                  << ": " << strerror(errno) << std::endl;
        free_slots_.push_back(slot);
        return;
    }

    group_slots_[group_nbo]       = slot;
    group_ref_counts_[group_nbo]  = 1;

    std::cout << "[GRE] Added group " << group_str
              << " → config_map[" << slot << "]" << std::endl;
}

void PacketReplicator::removeGroupDynamic(uint32_t group_nbo) {
    const std::string group_str = formatIpAddress(group_nbo);

    std::lock_guard<std::mutex> lock(group_mutex_);

    auto ref_it = group_ref_counts_.find(group_nbo);
    if (ref_it == group_ref_counts_.end()) return;

    // Decrement — only remove when the last subscriber leaves
    if (--ref_it->second > 0) return;

    // Zero the BPF map slot so the verifier loop stops matching this group
    auto slot_it = group_slots_.find(group_nbo);
    if (slot_it != group_slots_.end()) {
        struct { uint32_t target_ip; uint16_t target_port; uint16_t padding; } zero{};
        bpf_map_update_elem(config_map_fd_, &slot_it->second, &zero, BPF_ANY);
        free_slots_.push_back(slot_it->second);
        group_slots_.erase(slot_it);
    }

    group_ref_counts_.erase(ref_it);
    std::cout << "[GRE] Removed group " << group_str << std::endl;
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
    // mutex that the packet-processing hot path acquires via getCachedGroupDestinations().
    triggerArpResolution(ipAddress);

    Destination dest(ipAddress, port);
    if (!getDestinationMac(ipAddress, dest.mac)) {
        std::cerr << "Warning: ARP not resolved for " << ipAddress
                  << " — using broadcast MAC until next addDestination call" << std::endl;
        // dest.mac already set to 0xFF:FF:FF:FF:FF:FF in constructor
    }

    std::lock_guard<std::mutex> lock(destinations_mutex_);
    all_destinations_.emplace(ipAddress, dest);
    std::cout << "Added destination: " << ipAddress << ":" << port << std::endl;
}

void PacketReplicator::removeDestination(const std::string& ipAddress, uint16_t port) {
    std::vector<uint32_t> orphaned_groups;
    {
        std::lock_guard<std::mutex> lock(destinations_mutex_);

        all_destinations_.erase(ipAddress);

        // Remove from all per-group destination maps; collect now-empty groups
        for (auto& [group_nbo, subs] : group_destinations_) {
            subs.erase(ipAddress);
            if (subs.empty())
                orphaned_groups.push_back(group_nbo);
        }
        for (uint32_t g : orphaned_groups)
            group_destinations_.erase(g);
    }

    // Release destinations_mutex_ before removeGroupDynamic (acquires group_mutex_).
    // Without this, groups whose last subscriber was removed via CTRL_REMOVE_DESTINATION
    // would permanently consume a config_map slot and never return it to free_slots_,
    // exhausting the 16-slot limit over time.
    for (uint32_t g : orphaned_groups)
        removeGroupDynamic(g);

    std::cout << "Removed destination: " << ipAddress << ":" << port << std::endl;
}

std::vector<PacketReplicator::Destination> PacketReplicator::getDestinations() const {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    std::vector<Destination> result;
    result.reserve(all_destinations_.size());
    for (const auto& [ip, dest] : all_destinations_) {
        result.push_back(dest);
    }
    return result;
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

        // Clear BPF group tracking tables
        {
            std::lock_guard<std::mutex> lock(group_mutex_);
            group_slots_.clear();
            group_ref_counts_.clear();
            free_slots_.clear();
        }

        // Clear subscriber routing tables so a restart begins clean
        {
            std::lock_guard<std::mutex> lock(destinations_mutex_);
            group_destinations_.clear();
            all_destinations_.clear();
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
        all_destinations_.size()
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
    
    // HFT OPTIMIZATION: Pre-allocate batch vectors with cache-aligned memory.
    // GRE mode: exchange sends multi-hundred-frame bursts — use 256 to drain in one peek.
    // Unicast mode: sparse arrivals; 64 is never the limiting factor.
    // 256 fits well within the 2048-frame RX UMEM partition (256 in-flight + 1792 in fill queue).
    const int rx_batch = gre_mode_ ? 256 : 64;
    alignas(64) std::vector<int> offsets(rx_batch);
    alignas(64) std::vector<int> lengths(rx_batch);
    
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
    const uint8_t* payload_data = nullptr;
    size_t payload_len = 0;
    uint32_t group_nbo = 0;

    if (!extractUdpPayload(packetData, packetLen, payload_data, payload_len, group_nbo)) {
        return 0; // Not a valid UDP packet
    }

    // GRE mode: stamp feeder RX time into inner UDP payload[16..23] (big-endian uint64_t).
    // payload_data points to the inner IPv4 header; UDP payload starts at ihl*4 + 8 bytes in.
    // The sender reserved this slot (zeroed in the UMEM template). The receiver uses it to
    // split reported latency into hop1 (exchange→feeder) and hop2 (feeder→subscriber).
    if (gre_mode_) {
        const struct iphdr* inner = reinterpret_cast<const struct iphdr*>(payload_data);
        size_t udp_payload_off = static_cast<size_t>(inner->ihl) * 4 + sizeof(struct udphdr);
        if (payload_len >= udp_payload_off + 24) {  // HDR_SIZE = 24
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t feeder_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                                 + static_cast<uint64_t>(ts.tv_nsec);
            feeder_ns = __builtin_bswap64(feeder_ns);  // to big-endian (x86 is LE)
            memcpy(const_cast<uint8_t*>(payload_data) + udp_payload_off + 16, &feeder_ns, 8);
        }
    }

    // Per-group fan-out: only send to subscribers that joined this multicast group via IGMP.
    // Const ref to thread-local per-group cache — no copy, no lock on hot path.
    const std::vector<Destination>& current_destinations = getCachedGroupDestinations(group_nbo);
    if (current_destinations.empty()) {
        return 0;
    }

    int sent_count = 0;
    for (const auto& dest : current_destinations) {
        if (sendToDestinationWithQueue(dest, payload_data, payload_len, queueId)) {
            sent_count++;
            packets_sent_++;
            bytes_sent_ += payload_len;
        }
    }

    // One driver kick after all K subscribers have been queued — avoids K-1 redundant
    // needs_wakeup checks and potential sendto syscalls inside the per-destination loop.
    if (sent_count > 0)
        xdp_sockets_[queueId]->requestDriverPoll();

    return sent_count;
}

bool PacketReplicator::extractUdpPayloadGre(const uint8_t* packetData, size_t packetLen,
                                             const uint8_t*& payloadData, size_t& payloadLen,
                                             uint32_t& group_nbo) {
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

    // Mirror gre_filter.c line 148: only accept multicast inner destinations (224.0.0.0/4).
    // gre_filter.c enforces this before redirecting to AF_XDP, so non-multicast frames
    // should never arrive here; the check is a defence-in-depth guard.
    if ((ntohl(inner_ip->daddr) & 0xF0000000U) != 0xE0000000U)
        return false;

    size_t inner_ip_len = inner_ip->ihl * 4;
    if (inner_ip_len < 20 || inner_ip_len > 60)
        return false;

    // Return the inner IP datagram verbatim (inner IPv4 + UDP + payload).
    // The feeder re-encapsulates it in a new outer GRE unicast to each subscriber,
    // so the subscriber receives the original multicast UDP packet intact.
    size_t inner_datagram_len = ntohs(inner_ip->tot_len);
    if (inner_datagram_len < inner_ip_len + sizeof(struct udphdr))
        return false;
    if (inner_ip_offset + inner_datagram_len > packetLen)
        return false;

    payloadData = packetData + inner_ip_offset;   // points to inner IPv4 header
    payloadLen  = inner_datagram_len;
    group_nbo   = inner_ip->daddr;                // inner multicast group address
    return true;
}

bool PacketReplicator::extractUdpPayload(const uint8_t* packetData, size_t packetLen,
                                         const uint8_t*& payloadData, size_t& payloadLen,
                                         uint32_t& group_nbo) {
    if (gre_mode_)
        return extractUdpPayloadGre(packetData, packetLen, payloadData, payloadLen, group_nbo);

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

    // Defence-in-depth: BPF gates on listen_ip_nbo_, but verify here too so
    // unexpected frames don't produce misleading group_nbo values.
    if (ip->daddr != listen_ip_nbo_) {
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
    group_nbo  = ip->daddr;  // outer destination = multicast group (or unicast IP in unicast mode)

    return true;
}

bool PacketReplicator::sendToDestinationFallback(const Destination& destination, const uint8_t* data, size_t length) {
    if (gre_mode_) {
        // GRE fallback: prepend minimal 4-byte GRE header; kernel builds outer IP.
        // output_socket_ was created as SOCK_RAW/IPPROTO_GRE in initialize().
        uint8_t gre_buf[4 + 65535];
        if (length > sizeof(gre_buf) - 4) return false;
        gre_buf[0] = 0x00; gre_buf[1] = 0x00;  // flags = 0
        gre_buf[2] = 0x08; gre_buf[3] = 0x00;  // protocol = ETH_P_IP
        memcpy(gre_buf + 4, data, length);

        struct sockaddr_in dest = destination.addr;
        dest.sin_port = 0;  // ignored by raw sockets
        ssize_t sent = sendto(output_socket_, gre_buf, 4 + length, 0,
                              reinterpret_cast<const struct sockaddr*>(&dest), sizeof(dest));
        if (sent < 0) {
            std::cerr << "GRE fallback send failed to " << destination.ip_address
                      << ": " << strerror(errno) << std::endl;
            return false;
        }
        return sent == static_cast<ssize_t>(4 + length);
    }

    // Non-GRE: regular UDP socket, data is the UDP payload
    ssize_t sent = sendto(output_socket_, data, length, 0,
                          reinterpret_cast<const struct sockaddr*>(&destination.addr),
                          sizeof(destination.addr));
    if (sent < 0) {
        std::cerr << "Fallback send failed to " << destination.ip_address << ":" << destination.port
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
    
    // Drain completions first to free ring slots (ena-xdp pattern)
    xdp_socket->pollTxCompletions();

    // Reserve a TX ring slot first; derive the UMEM frame address from the ring index.
    // This guarantees the frame is not still in-flight: a ring slot is only reusable
    // after its completion has been processed, so (tx_idx % TX_FRAMES) * FRAME_SIZE
    // maps to a frame that is safe to write into.
    uint32_t tx_idx = 0;
    int ret = xdp_socket->reserveTxRing(1, &tx_idx);
    if (ret != 1) {
        // TX ring full — kick driver to flush pending completions and retry once.
        // If the ring is still full after the retry, throw so sendToDestinationWithQueue
        // can fall back to the kernel socket instead of silently dropping the packet.
        xdp_socket->requestDriverPoll();
        xdp_socket->pollTxCompletions();
        ret = xdp_socket->reserveTxRing(1, &tx_idx);
        if (ret != 1) {
            DEBUG_TX_PRINT("DEBUG TX: TX ring full after retry, falling back");
            throw std::runtime_error("TX ring full after retry");
        }
    }

    // Frame address derived from ring slot — power-of-2 modulo via bitmask
    static constexpr uint64_t FRAME_SIZE     = 4096;
    static constexpr uint64_t TX_FRAMES_MASK = AFXDPSocket::DEFAULT_TX_FRAMES - 1;
    uint64_t tx_frame_addr = (static_cast<uint64_t>(tx_idx) & TX_FRAMES_MASK) * FRAME_SIZE;

    DEBUG_TX_PRINT("DEBUG TX: tx_idx=" << tx_idx << ", tx_frame_addr=0x"
              << std::hex << tx_frame_addr << std::dec);

    // Build outgoing packet: GRE-wrapped inner IP datagram in GRE mode, plain UDP otherwise
    uint8_t* tx_buffer = xdp_socket->getUmemBuffer() + tx_frame_addr;
    size_t packet_len = gre_mode_
        ? createGrePacket(destination, data, length, tx_buffer, FRAME_SIZE)
        : createUdpPacket(destination, data, length, tx_buffer, FRAME_SIZE);
    if (packet_len == 0) {
        DEBUG_TX_PRINT("DEBUG TX: packet build failed!");
        return false;
    }

    DEBUG_TX_PRINT("DEBUG TX: Created packet, len=" << packet_len);

    // Fill TX descriptor (ena-xdp pattern)
    xdp_socket->setTxDescriptor(tx_idx, tx_frame_addr, packet_len);

    DEBUG_TX_PRINT("DEBUG TX: Set TX descriptor, addr=0x" << std::hex << tx_frame_addr
              << std::dec << ", len=" << packet_len);
    
    // Submit TX ring entry — driver kick is batched at the replicatePacket() call site
    // so the wakeup is issued once for all K subscribers rather than K times.
    xdp_socket->submitTxRing(1);

    DEBUG_TX_PRINT("DEBUG TX: Submitted to TX ring");

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
    ip->check    = 0;  // must be zero before checksum computation
    inet_aton(cached_iface_ip_.c_str(), (struct in_addr*)&ip->saddr);
    ip->daddr    = destination.addr.sin_addr.s_addr;

    // Calculate IP checksum (RFC 1071)
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

size_t PacketReplicator::createGrePacket(const Destination& destination,
                                          const uint8_t* inner_ip, size_t inner_ip_len,
                                          uint8_t* buffer, size_t bufferSize) {
    // Layout: Eth(14) + outer IPv4(20) + GRE(4) + inner IP datagram
    static constexpr size_t GRE_HDR_LEN    = 4;
    static constexpr size_t OUTER_IP_LEN   = sizeof(struct iphdr);
    static constexpr size_t ETH_LEN        = sizeof(struct ethhdr);
    size_t total_len = ETH_LEN + OUTER_IP_LEN + GRE_HDR_LEN + inner_ip_len;

    if (total_len > bufferSize) {
        std::cerr << "GRE packet too large for buffer: " << total_len << " > " << bufferSize << std::endl;
        return 0;
    }

    // Ethernet header
    struct ethhdr* eth = reinterpret_cast<struct ethhdr*>(buffer);
    memcpy(eth->h_dest,   destination.mac,   ETH_ALEN);
    memcpy(eth->h_source, cached_iface_mac_, ETH_ALEN);
    eth->h_proto = htons(ETH_P_IP);

    // Outer IPv4 header — unicast feeder → subscriber
    struct iphdr* ip = reinterpret_cast<struct iphdr*>(buffer + ETH_LEN);
    ip->version  = 4;
    ip->ihl      = 5;
    ip->tos      = 0;
    ip->tot_len  = htons(OUTER_IP_LEN + GRE_HDR_LEN + inner_ip_len);
    ip->id       = 0;
    ip->frag_off = 0;           // GRE frames may need fragmentation; don't set DF
    ip->ttl      = 64;
    ip->protocol = IPPROTO_GRE; // 47
    ip->check    = 0;
    inet_aton(cached_iface_ip_.c_str(), reinterpret_cast<struct in_addr*>(&ip->saddr));
    ip->daddr = destination.addr.sin_addr.s_addr;

    uint32_t sum = 0;
    const uint16_t* ip_words = reinterpret_cast<const uint16_t*>(ip);
    for (int i = 0; i < 10; i++) sum += ip_words[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->check = static_cast<uint16_t>(~sum);

    // GRE header: flags=0, protocol=0x0800 (IPv4 inner)
    uint8_t* gre = buffer + ETH_LEN + OUTER_IP_LEN;
    gre[0] = 0x00; gre[1] = 0x00;  // flags word (no checksum / key / seq)
    gre[2] = 0x08; gre[3] = 0x00;  // protocol = ETH_P_IP

    // Inner IP datagram verbatim (preserves multicast dst IP, UDP ports, payload)
    memcpy(buffer + ETH_LEN + OUTER_IP_LEN + GRE_HDR_LEN, inner_ip, inner_ip_len);

    return total_len;
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
        
        case CTRL_MCAST_JOIN: {
            // [4][4B group IP NBO]
            // Subscriber IP is inferred from the UDP source address (clientAddr).
            // Only valid in GRE mode; in unicast mode use CTRL_ADD_DESTINATION instead.
            // No port in the wire format: inner UDP dst is preserved verbatim from the
            // exchange, so subscribers always receive on listen_port_.
            if (!gre_mode_) {
                std::cerr << "Control: MCAST_JOIN ignored in unicast mode — use ADD_DESTINATION\n";
                response.push_back(0);
                break;
            }
            if (messageLen >= 5) {
                uint32_t group_ip;
                memcpy(&group_ip, message + 1, 4);

                std::string subscriber_ip(client_ip);
                std::cout << "Control: MCAST_JOIN group=" << formatIpAddress(group_ip)
                          << " subscriber=" << subscriber_ip << std::endl;

                try {
                    // ARP resolution outside any lock
                    triggerArpResolution(subscriber_ip);
                    Destination dest(subscriber_ip, listen_port_);
                    getDestinationMac(subscriber_ip, dest.mac);

                    // Only call addGroupDynamic (which increments ref count) if this
                    // subscriber is not already registered for the group.  A re-join
                    // with a different port should update the destination without
                    // double-counting the reference, which would leave the BPF slot
                    // live after the subscriber sends a single MCAST_LEAVE.
                    bool already_in_group = false;
                    {
                        std::lock_guard<std::mutex> lock(destinations_mutex_);
                        auto git = group_destinations_.find(group_ip);
                        if (git != group_destinations_.end() && git->second.count(subscriber_ip))
                            already_in_group = true;
                    }
                    if (!already_in_group)
                        addGroupDynamic(group_ip);

                    // Register subscriber for this specific group+port
                    {
                        std::lock_guard<std::mutex> lock(destinations_mutex_);
                        group_destinations_[group_ip].insert_or_assign(subscriber_ip, dest);
                        all_destinations_.insert_or_assign(subscriber_ip, dest);
                    }
                    response.push_back(1);
                } catch (const std::exception& e) {
                    std::cerr << "MCAST_JOIN failed: " << e.what() << std::endl;
                    response.push_back(0);
                }
            }
            break;
        }

        case CTRL_MCAST_LEAVE: {
            // [5][4B group IP NBO]
            if (messageLen >= 5) {
                uint32_t group_ip;
                memcpy(&group_ip, message + 1, 4);

                std::string subscriber_ip(client_ip);
                std::cout << "Control: MCAST_LEAVE group=" << formatIpAddress(group_ip)
                          << " subscriber=" << subscriber_ip << std::endl;

                bool last_subscriber = false;
                {
                    std::lock_guard<std::mutex> lock(destinations_mutex_);
                    auto git = group_destinations_.find(group_ip);
                    if (git != group_destinations_.end()) {
                        git->second.erase(subscriber_ip);
                        if (git->second.empty()) {
                            group_destinations_.erase(git);
                            last_subscriber = true;
                        }
                    }
                    // Remove from all_destinations_ only if this subscriber is no
                    // longer in any group.  A subscriber registered for N groups
                    // sends N MCAST_LEAVE messages; premature removal here would
                    // make control_client list show them as gone while they are
                    // still receiving traffic for the remaining groups.
                    bool still_in_group = false;
                    for (const auto& [g, subs] : group_destinations_) {
                        if (subs.count(subscriber_ip)) { still_in_group = true; break; }
                    }
                    if (!still_in_group)
                        all_destinations_.erase(subscriber_ip);
                }
                // Release destinations_mutex_ before removeGroupDynamic (uses group_mutex_)
                if (last_subscriber)
                    removeGroupDynamic(group_ip);

                response.push_back(1);
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
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return std::string(buf);
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
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
    ip_address = buf;
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

bool PacketReplicator::getDestinationMac(const std::string& ip_address, uint8_t* mac_address) {
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

// HFT OPTIMIZATION IMPLEMENTATIONS

// Thread-local destination cache definition
thread_local PacketReplicator::ThreadLocalDestCache PacketReplicator::dest_cache_;

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

const std::vector<PacketReplicator::Destination>& PacketReplicator::getCachedGroupDestinations(uint32_t group_nbo) {
    auto now = std::chrono::steady_clock::now();
    if (!dest_cache_.valid ||
        (now - dest_cache_.last_update) > ThreadLocalDestCache::CACHE_TIMEOUT) {
        updateDestinationCache();
    }
    static const std::vector<Destination> empty;
    auto it = dest_cache_.group_dests.find(group_nbo);
    if (it == dest_cache_.group_dests.end()) return empty;
    return it->second;  // const ref — zero copy on hot path
}

void PacketReplicator::updateDestinationCache() {
    std::unordered_map<uint32_t, std::unordered_map<std::string, Destination>> gd_copy;
    std::unordered_map<std::string, Destination> all_copy;
    {
        std::lock_guard<std::mutex> lock(destinations_mutex_);
        gd_copy  = group_destinations_;
        all_copy = all_destinations_;
    }

    dest_cache_.group_dests.clear();

    // GRE mode: per-group fan-out from group_destinations_
    for (const auto& [group_nbo, subs] : gd_copy) {
        auto& vec = dest_cache_.group_dests[group_nbo];
        vec.reserve(subs.size());
        for (const auto& [ip, dest] : subs)
            vec.push_back(dest);
    }

    // Unicast mode: all_destinations_ subscribers keyed by listen_ip_nbo_
    if (!gre_mode_ && !all_copy.empty()) {
        auto& vec = dest_cache_.group_dests[listen_ip_nbo_];
        vec.reserve(all_copy.size());
        for (const auto& [ip, dest] : all_copy)
            vec.push_back(dest);
    }

    // Re-resolve any broadcast MACs (ARP not yet resolved at add time).
    // Once resolved the fast path resumes without removeDestination/addDestination.
    static constexpr uint8_t BROADCAST_MAC[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    for (auto& [group_nbo, dests] : dest_cache_.group_dests) {
        for (auto& dest : dests) {
            if (memcmp(dest.mac, BROADCAST_MAC, ETH_ALEN) == 0)
                getDestinationMac(dest.ip_address, dest.mac);
        }
    }

    dest_cache_.last_update = std::chrono::steady_clock::now();
    dest_cache_.valid = true;
}

