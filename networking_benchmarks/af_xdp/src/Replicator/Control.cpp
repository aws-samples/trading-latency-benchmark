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

// ReplicatorControl.cpp — the binary UDP control protocol (add/remove/list/
// mcast-join/leave), its receive loop, and the upstream control-multicast →
// producer forwarding path.

#include "Internal.hpp"

void Replicator::setUpstreamControl(const std::string& ctrlGroup, uint16_t ctrlPort,
                                          const std::string& producerIp, uint16_t producerPort) {
    ctrl_multicast_group_ = ctrlGroup;
    ctrl_multicast_port_  = ctrlPort;
    producer_ip_          = producerIp;
    producer_port_        = producerPort;
}

void Replicator::joinControlMulticastGroup() {
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

void Replicator::handleUpstreamControl() {
    std::cout << "Upstream control thread started: " << ctrl_multicast_group_ << ":"
              << ctrl_multicast_port_ << " → " << producer_ip_ << ":" << producer_port_ << std::endl;

    struct sockaddr_in producer_addr{};
    producer_addr.sin_family = AF_INET;
    producer_addr.sin_port   = htons(producer_port_);
    inet_aton(producer_ip_.c_str(), &producer_addr.sin_addr);

    // 100ms recv timeout so the loop checks running_ promptly. stop() joins this
    // thread, so the timeout bounds how long shutdown blocks here. Not in the
    // packet path, so the extra idle wakeups cost nothing measurable.
    struct timeval tv{0, 100000};
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

void Replicator::handleControlProtocol() {
    std::cout << "Control protocol thread started on port " << CONTROL_PORT << std::endl;
    
    std::vector<uint8_t> buffer(1024);
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    // 100ms recv timeout so the loop checks running_ promptly; stop() joins this
    // thread, so this bounds how long shutdown blocks here.
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
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

std::vector<uint8_t> Replicator::processControlMessage(const uint8_t* message, size_t messageLen, 
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
            // Destination IP is inferred from the UDP source address (clientAddr).
            // Only valid in mcast mode; in unicast mode use CTRL_ADD_DESTINATION instead.
            // No port in the wire format: inner UDP dst is preserved verbatim from the
            // source, so destinations always receive on listen_port_.
            if (!mcast_mode_) {
                std::cerr << "Control: MCAST_JOIN ignored in unicast mode — use ADD_DESTINATION\n";
                response.push_back(0);
                break;
            }
            if (messageLen >= 5) {
                uint32_t group_ip;
                memcpy(&group_ip, message + 1, 4);

                std::string destination_ip(client_ip);
                std::cout << "Control: MCAST_JOIN group=" << formatIpAddress(group_ip)
                          << " destination=" << destination_ip << std::endl;

                try {
                    // ARP resolution outside any lock
                    triggerArpResolution(destination_ip);
                    Destination dest(destination_ip, listen_port_);
                    getDestinationMac(destination_ip, dest.mac);

                    // Only call addGroupDynamic (which increments ref count) if this
                    // destination is not already registered for the group.  A re-join
                    // with a different port should update the destination without
                    // double-counting the reference, which would leave the BPF slot
                    // live after the destination sends a single MCAST_LEAVE.
                    bool already_in_group = false;
                    {
                        std::lock_guard<std::mutex> lock(destinations_mutex_);
                        auto git = group_destinations_.find(group_ip);
                        if (git != group_destinations_.end() && git->second.count(destination_ip))
                            already_in_group = true;
                    }
                    if (!already_in_group)
                        addGroupDynamic(group_ip);

                    // Register destination for this specific group+port
                    {
                        std::lock_guard<std::mutex> lock(destinations_mutex_);
                        group_destinations_[group_ip].insert_or_assign(destination_ip, dest);
                        all_destinations_.insert_or_assign(destination_ip, dest);
                    }
                    // bpf_tx forward mode: push this destination into fwd_map so
                    // mcast.o XDP_TX-forwards the group in-kernel (single dest/group).
                    updateBpfTxFwdTarget(group_ip, dest, true);
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

                std::string destination_ip(client_ip);
                std::cout << "Control: MCAST_LEAVE group=" << formatIpAddress(group_ip)
                          << " destination=" << destination_ip << std::endl;

                bool last_destination = false;
                {
                    std::lock_guard<std::mutex> lock(destinations_mutex_);
                    auto git = group_destinations_.find(group_ip);
                    if (git != group_destinations_.end()) {
                        git->second.erase(destination_ip);
                        if (git->second.empty()) {
                            group_destinations_.erase(git);
                            last_destination = true;
                        }
                    }
                    // Remove from all_destinations_ only if this destination is no
                    // longer in any group.  A destination registered for N groups
                    // sends N MCAST_LEAVE messages; premature removal here would
                    // make ctl list show them as gone while they are
                    // still receiving traffic for the remaining groups.
                    bool still_in_group = false;
                    for (const auto& [g, subs] : group_destinations_) {
                        if (subs.count(destination_ip)) { still_in_group = true; break; }
                    }
                    if (!still_in_group)
                        all_destinations_.erase(destination_ip);
                }
                // Release destinations_mutex_ before removeGroupDynamic (uses group_mutex_)
                if (last_destination)
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
