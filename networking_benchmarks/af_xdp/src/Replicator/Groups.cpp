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
// config_map (ref-counted slot alloc/free).

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

    // kernel fwd mode has no XDP program loaded, so there is no config_map to
    // allocate a slot from — group filtering happens entirely in userspace via
    // getCachedGroupDestinations(). Ref-counting still runs (above/below) since
    // Control.cpp's CTRL_MCAST_LEAVE depends on it regardless of fwd mode; only
    // the BPF slot write is skipped. This also means kernel fwd mode has no
    // MAX_GROUPS=16 ceiling — group_ref_counts_ is an unordered_map, not a
    // fixed-size array.
    if (kernelFwdActive()) {
        group_ref_counts_[group_nbo] = 1;
        std::cout << "[mcast] Added group " << group_str << " (kernel fwd mode, no config_map slot)" << std::endl;
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

    // kernel fwd mode: no config_map slot was ever allocated for this group
    // (see addGroupDynamic) — just drop the ref-count entry.
    if (kernelFwdActive()) {
        group_ref_counts_.erase(ref_it);
        std::cout << "[mcast] Removed group " << group_str << " (kernel fwd mode)" << std::endl;
        return;
    }

    // Zero the BPF map slot so the verifier loop stops matching this group
    auto slot_it = group_slots_.find(group_nbo);
    if (slot_it != group_slots_.end()) {
        struct { uint32_t target_ip; uint16_t target_port; uint16_t padding; } zero{};
        bpf_map_update_elem(config_map_fd_, &slot_it->second, &zero, BPF_ANY);
        free_slots_.push_back(slot_it->second);
        group_slots_.erase(slot_it);
    }

    group_ref_counts_.erase(ref_it);
    std::cout << "[mcast] Removed group " << group_str << std::endl;
}
