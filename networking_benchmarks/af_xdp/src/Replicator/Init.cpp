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

// ReplicatorInit.cpp — one-time setup: XDP program selection/load, per-queue
// AF_XDP socket creation, control/output socket bring-up, interface caching,
// and BPF config_map seeding (configureXdpProgram).

#include "Internal.hpp"

void Replicator::initialize(bool useZeroCopy) {
    std::cout << "Initializing Replicator with zero-copy: " << (useZeroCopy ? "enabled" : "disabled") << std::endl;
    
    // Set resource limits for AF_XDP
    XdpSocket::setResourceLimits();
    
    // Create AF_XDP sockets for all queues
    xdp_sockets_.resize(num_queues_);
    
    // Select XDP program:
    //   mcast_mode_ → mcast.o  (m2u-tagged unicast carries the multicast group)
    //   otherwise  → ucast.o  (direct unicast feed)
    const char* xdp_filename = mcast_mode_ ? "mcast.o" : "ucast.o";
    static const std::string search_paths[] = {
        std::string("./src/xdp/") + xdp_filename,    // dev: running from af_xdp/ source tree
        std::string("./xdp/") + xdp_filename,        // installed: running from /opt/af-xdp/
        std::string("/opt/af-xdp/xdp/") + xdp_filename,  // absolute: baked AMI
    };
    std::string xdp_program_path;
    for (const auto& p : search_paths) {
        if (access(p.c_str(), R_OK) == 0) { xdp_program_path = p; break; }
    }
    if (xdp_program_path.empty()) {
        throw std::runtime_error(
            std::string("XDP program ") + xdp_filename +
            " not found in search paths (./src/xdp/, ./xdp/, /opt/af-xdp/xdp/)");
    }
    std::cout << "Loading XDP program: " << xdp_program_path
              << (mcast_mode_ ? " (m2u multicast mode)" : "") << std::endl;
    XdpSocket::loadXdpProgram(listen_interface_, xdp_program_path, useZeroCopy);

    // Cache listen_ip_ as NBO for use by configureXdpProgram() and updateDestinationCache()
    listen_ip_nbo_ = parseIpAddress(listen_ip_);

    // Configure XDP program with target IP and port
    configureXdpProgram();

    // In mcast mode seed the multicast group immediately into config_map slot 0
    // so the BPF filter starts redirecting frames before any destination joins.
    if (mcast_mode_) {
        addGroupDynamic(listen_ip_nbo_);
    }
    
    // Create and configure AF_XDP socket for each queue
    int xdp_flags = useZeroCopy ? XdpSocket::XDP_FLAGS_ZERO_COPY : XdpSocket::XDP_FLAGS_DRV_MODE;
    
    for (int queue_id = 0; queue_id < num_queues_; queue_id++) {
        std::cout << "Creating AF_XDP socket for queue " << queue_id << std::endl;
        
        // Create AF_XDP socket for this queue with proper frame count (following ena-xdp)
        xdp_sockets_[queue_id] = std::make_unique<XdpSocket>(4096, XdpSocket::DEFAULT_UMEM_FRAMES, 0);
        
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
    // Kernel fallback socket (unresolved ARP / edge cases). Both ucast and the
    // light m2u mcast tunnel are plain unicast UDP now, so a datagram socket
    // suffices — the m2u header travels inside the UDP payload.
    output_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (output_socket_ < 0) {
        throw std::runtime_error("Failed to create output socket: " + std::string(strerror(errno)));
    }

    // Cache interface IP and MAC once — createUdpPacket() reads these on every TX packet
    if (!getInterfaceIp(listen_interface_, cached_iface_ip_))
        throw std::runtime_error("Failed to get IP for interface " + listen_interface_);
    // Parse the source IP once here — createUdpPacket() reused it via inet_aton()
    // (a string parse) on every TX packet; cache the network-order value instead.
    inet_aton(cached_iface_ip_.c_str(), reinterpret_cast<struct in_addr*>(&cached_iface_saddr_nbo_));
    if (!getInterfaceMac(listen_interface_, cached_iface_mac_))
        throw std::runtime_error("Failed to get MAC for interface " + listen_interface_);
    std::cout << "Interface " << listen_interface_
              << " IP=" << cached_iface_ip_ << std::endl;

    // Forward path selector (REPLICATOR_FWD_MODE): copy (default) | inplace | kernel.
    if (const char* fm = getenv("REPLICATOR_FWD_MODE")) {
        if      (strcmp(fm, "inplace") == 0) fwd_mode_ = 1;
        else if (strcmp(fm, "kernel")  == 0) fwd_mode_ = 2;
        else                                 fwd_mode_ = 0;
    }
    std::cout << "Forward mode: "
              << (fwd_mode_ == 2 ? "kernel (XDP_TX)" : fwd_mode_ == 1 ? "inplace (zero-copy)" : "copy")
              << std::endl;

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

    std::cout << "Replicator initialized successfully with " << num_queues_ << " queues" << std::endl;
}

void Replicator::configureXdpProgram() {
    static constexpr int MAX_GROUPS = 16;

    config_map_fd_ = XdpSocket::getXdpMapFd("config_map");
    if (config_map_fd_ < 0) {
        throw std::runtime_error("Could not find config_map in loaded XDP program — cannot configure filter");
    }

    // fwd_map is only used by REPLICATOR_FWD_MODE=kernel; absence is non-fatal
    // (an older mcast.o without it just can't do kernel forward).
    fwd_map_fd_ = XdpSocket::getXdpMapFd("fwd_map");
    if (fwd_mode_ == 2 && fwd_map_fd_ < 0) {
        std::cerr << "[mcast] REPLICATOR_FWD_MODE=kernel but fwd_map not found in XDP program; "
                     "falling back to userspace copy forward" << std::endl;
        fwd_mode_ = 0;
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
    // mcast mode: slot 0 (and others) are populated dynamically by addGroupDynamic().
    int first_free_slot = 0;
    if (!mcast_mode_) {
        unicast_config cfg{};
        cfg.target_ip   = listen_ip_nbo_;
        cfg.target_port = htons(listen_port_);
        bpf_map_update_elem(config_map_fd_, &first_free_slot, &cfg, BPF_ANY);
        first_free_slot = 1;  // slot 0 is taken; dynamic alloc starts from slot 1
        std::cout << "Unicast filter: seeded slot 0 with " << listen_ip_ << ":" << listen_port_ << std::endl;
    }

    // Initialise the free-slot pool (mcast: all 16 slots; unicast: slots 1-15)
    free_slots_.clear();
    for (int i = MAX_GROUPS - 1; i >= first_free_slot; --i)
        free_slots_.push_back(static_cast<uint32_t>(i));
}
