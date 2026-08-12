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

// ReplicatorGroups.cpp — dynamic multicast-group lifecycle against the BPF
// config_map (ref-counted slot alloc/free) plus the kernel XDP_TX forward
// target (fwd_map) used by REPLICATOR_FWD_MODE=kernel.

#include "Internal.hpp"

void Replicator::addGroupDynamic(uint32_t group_nbo) {
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
        std::cerr << "[mcast] config_map full (max 16 groups); ignoring Join for "
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
        std::cerr << "[mcast] bpf_map_update_elem failed for " << group_str
                  << ": " << strerror(errno) << std::endl;
        free_slots_.push_back(slot);
        return;
    }

    group_slots_[group_nbo]       = slot;
    group_ref_counts_[group_nbo]  = 1;

    std::cout << "[mcast] Added group " << group_str
              << " → config_map[" << slot << "]" << std::endl;
}

void Replicator::removeGroupDynamic(uint32_t group_nbo) {
    const std::string group_str = formatIpAddress(group_nbo);

    std::lock_guard<std::mutex> lock(group_mutex_);

    auto ref_it = group_ref_counts_.find(group_nbo);
    if (ref_it == group_ref_counts_.end()) return;

    // Decrement — only remove when the last destination leaves
    if (--ref_it->second > 0) return;

    // Zero the BPF map slot so the verifier loop stops matching this group
    auto slot_it = group_slots_.find(group_nbo);
    if (slot_it != group_slots_.end()) {
        struct { uint32_t target_ip; uint16_t target_port; uint16_t padding; } zero{};
        bpf_map_update_elem(config_map_fd_, &slot_it->second, &zero, BPF_ANY);
        // Kernel-fwd mode: also clear the forward target for this slot. Slots are
        // recycled via free_slots_, so a stale fwd_map entry (enabled=1, old dest)
        // would otherwise XDP_TX a *reused* slot's group to the previous
        // destination. Zeroing disables it (enabled=0) until the next join.
        if (fwd_map_fd_ >= 0) {
            struct { uint8_t d[28]; } zero_ft{};  // matches struct fwd_target (28 bytes)
            bpf_map_update_elem(fwd_map_fd_, &slot_it->second, &zero_ft, BPF_ANY);
        }
        free_slots_.push_back(slot_it->second);
        group_slots_.erase(slot_it);
    }

    group_ref_counts_.erase(ref_it);
    std::cout << "[mcast] Removed group " << group_str << std::endl;
}

void Replicator::updateKernelFwdTarget(uint32_t group_nbo, const Destination& dest, bool enable) {
    if (fwd_mode_ != 2 || fwd_map_fd_ < 0) return;

    // Kernel forward mode (REPLICATOR_FWD_MODE=kernel) forwards 1:1 via XDP_TX.
    // Fan-out to multiple destinations is not possible in this mode; refuse to
    // enable it when more than one destination has joined the group. Use copy or
    // inplace mode for multi-destination workloads.
    if (enable) {
        size_t ndest = 0;
        {
            std::lock_guard<std::mutex> lock(destinations_mutex_);
            auto git = group_destinations_.find(group_nbo);
            if (git != group_destinations_.end()) ndest = git->second.size();
        }
        if (ndest > 1) {
            std::cerr << "[mcast] REFUSING kernel fwd target: " << ndest
                      << " destinations joined this group, but REPLICATOR_FWD_MODE=kernel"
                         " (XDP_TX) can only forward to ONE destination — the others would"
                         " silently receive nothing.\n"
                         "        Use REPLICATOR_FWD_MODE=copy or inplace for fan-out to"
                         " multiple destinations." << std::endl;
            return;
        }
    }

    uint32_t slot;
    {
        std::lock_guard<std::mutex> lock(group_mutex_);
        auto it = group_slots_.find(group_nbo);
        if (it == group_slots_.end()) return;   // group not in config_map yet
        slot = it->second;
    }
    // Layout must match struct fwd_target in src/xdp/mcast.c (28 bytes).
    struct fwd_target {
        uint8_t  dmac[6];
        uint8_t  smac[6];
        uint32_t dip;
        uint32_t sip;
        uint16_t dport;
        uint16_t sport;
        uint8_t  enabled;
        uint8_t  pad[3];
    } ft{};
    memcpy(ft.dmac, dest.mac, 6);
    memcpy(ft.smac, cached_iface_mac_, 6);
    ft.dip     = dest.addr.sin_addr.s_addr;
    ft.sip     = cached_iface_saddr_nbo_;
    ft.dport   = dest.addr.sin_port;
    ft.sport   = htons(listen_port_);
    ft.enabled = enable ? 1 : 0;
    if (bpf_map_update_elem(fwd_map_fd_, &slot, &ft, BPF_ANY) != 0) {
        std::cerr << "[mcast] fwd_map update failed for slot " << slot
                  << ": " << strerror(errno) << std::endl;
    } else {
        std::cout << "[mcast] kernel fwd target → fwd_map[" << slot << "] "
                  << dest.ip_address << ":" << dest.port
                  << (enable ? " (enabled)" : " (disabled)") << std::endl;
    }
}
