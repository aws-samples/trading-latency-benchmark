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

// ReplicatorDestinations.cpp — the Destination value type, the canonical
// destination registry (add/remove/list), and the per-thread destination
// cache that feeds the fan-out hot path.

#include "Internal.hpp"

// Destination implementation
Replicator::Destination::Destination(const std::string& ip, uint16_t p)
    : ip_address(ip), port(p) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(ip_address.c_str(), &addr.sin_addr) == 0) {
        throw std::invalid_argument("Invalid IP address: " + ip_address);
    }
    memset(mac, 0xFF, sizeof(mac));  // Default: broadcast; replaced after ARP resolution
}

bool Replicator::Destination::operator<(const Destination& other) const {
    if (ip_address != other.ip_address) {
        return ip_address < other.ip_address;
    }
    return port < other.port;
}

void Replicator::addDestination(const std::string& ipAddress, uint16_t port) {
    // ARP trigger and MAC lookup happen outside the lock: triggerArpResolution sleeps
    // 100ms and getDestinationMac reads /proc/net/arp — both unacceptable inside the
    // mutex that the packet-processing hot path acquires via getCachedGroupDestinations().
    Destination dest(ipAddress, port);

    // Resolve MAC with retries. A benchmark measurement with a broadcast-MAC
    // destination is silently invalid, so reject rather than continue.
    bool resolved = false;
    for (int attempt = 0; attempt < 3 && !resolved; ++attempt) {
        triggerArpResolution(ipAddress);
        if (getDestinationMac(ipAddress, dest.mac))
            resolved = true;
    }
    if (!resolved) {
        throw std::runtime_error("Failed to resolve MAC for " + ipAddress +
                                 " after 3 attempts - refusing to add destination");
    }

    std::lock_guard<std::mutex> lock(destinations_mutex_);
    all_destinations_.emplace(ipAddress, dest);
    std::cout << "Added destination: " << ipAddress << ":" << port << std::endl;
}

void Replicator::removeDestination(const std::string& ipAddress, uint16_t port) {
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
    // Without this, groups whose last destination was removed via CTRL_REMOVE_DESTINATION
    // would permanently consume a config_map slot and never return it to free_slots_,
    // exhausting the 16-slot limit over time.
    for (uint32_t g : orphaned_groups)
        removeGroupDynamic(g);

    std::cout << "Removed destination: " << ipAddress << ":" << port << std::endl;
}

std::vector<Replicator::Destination> Replicator::getDestinations() const {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    std::vector<Destination> result;
    result.reserve(all_destinations_.size());
    for (const auto& [ip, dest] : all_destinations_) {
        result.push_back(dest);
    }
    return result;
}

// Thread-local destination cache definition
thread_local Replicator::ThreadLocalDestCache Replicator::dest_cache_;

const std::vector<Replicator::Destination>& Replicator::getCachedGroupDestinations(uint32_t group_nbo) {
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

void Replicator::updateDestinationCache() {
    std::unordered_map<uint32_t, std::unordered_map<std::string, Destination>> gd_copy;
    std::unordered_map<std::string, Destination> all_copy;
    {
        std::lock_guard<std::mutex> lock(destinations_mutex_);
        gd_copy  = group_destinations_;
        all_copy = all_destinations_;
    }

    dest_cache_.group_dests.clear();

    // mcast mode: per-group fan-out from group_destinations_
    for (const auto& [group_nbo, subs] : gd_copy) {
        auto& vec = dest_cache_.group_dests[group_nbo];
        vec.reserve(subs.size());
        for (const auto& [ip, dest] : subs)
            vec.push_back(dest);
    }

    // Unicast mode: all_destinations_ destinations keyed by listen_ip_nbo_
    if (!mcast_mode_ && !all_copy.empty()) {
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
