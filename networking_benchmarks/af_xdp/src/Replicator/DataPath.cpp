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

// ReplicatorDataPath.cpp — the latency-critical datapath: per-queue RX busy
// poll, per-group fan-out/replication, UDP payload parse (plain + m2u decap),
// zero-copy AF_XDP TX, kernel fallback, and packet build / in-place patch.

#include "Internal.hpp"
#include "common/wire.h"   // S1: the ONE definition of the m2u wire format
#include <poll.h>
#include <linux/net_tstamp.h>  // SOF_TIMESTAMPING_RX_SOFTWARE / SOF_TIMESTAMPING_SOFTWARE (kernel fwd mode RX socket tuning)

void Replicator::processPacketsForQueue(int queueId) {
    std::cout << "HFT-optimized packet processing thread started for queue " << queueId << std::endl;
    
    // Batch size: 256 drains a mcast burst in one peek (source sends 100–256 frames).
    // 64 is enough for sparse ucast arrivals. Both fit within the 2048-frame RX UMEM.
    // S4: these are plain heap vectors sized once, outside the loop. The previous
    // code wrote `alignas(64) std::vector<int>` and claimed "cache-aligned memory",
    // which is not what that does — alignas applies to the ~24-byte vector object,
    // not to its heap buffer. Dropping the false claim rather than pretending:
    // XdpSocket::receive() takes std::vector<int>&, and for a small int array of
    // descriptor offsets the allocator's default alignment is not a hot-path factor.
    static constexpr int MAX_RX_BATCH = 256;
    const int rx_batch = mcast_mode_ ? MAX_RX_BATCH : 64;
    std::vector<int> offsets(rx_batch);
    std::vector<int> lengths(rx_batch);
    
    // tx_frames must be a power of 2 so (tx_idx & mask) * FRAME_SIZE gives the UMEM address.
    const uint32_t tx_frames = 2048;  // Must be power of 2
    static_assert((tx_frames & (tx_frames - 1)) == 0, "tx_frames must be power of 2");
    
    while (__builtin_expect(running_.load(std::memory_order_relaxed), 1)) {
        try {
            int received = xdp_sockets_[queueId]->receive(offsets, lengths);
            
            if (__builtin_expect(received > 0, 1)) {
                // Prefetch the second packet while processing the first.
                if (__builtin_expect(received > 1, 1)) {
                    uint8_t* next_packet = xdp_sockets_[queueId]->getUmemBuffer() + offsets[1];
                    __builtin_prefetch(next_packet, 0, 3);  // Prefetch for read, high temporal locality
                }
                
                // P4: accumulate in locals and publish ONCE per drained batch,
                // instead of three atomic RMWs per packet.
                uint64_t batch_bytes = 0;
                int      batch_sent  = 0;

                // Process each packet with optimized loop
                for (int i = 0; i < received; i++) {
                    uint8_t* packet_data = xdp_sockets_[queueId]->getUmemBuffer() + offsets[i];
                    size_t packet_len = lengths[i];

                    // Prefetch the next packet while processing the current one.
                    if (__builtin_expect(i + 1 < received, 1)) {
                        uint8_t* next_packet = xdp_sockets_[queueId]->getUmemBuffer() + offsets[i + 1];
                        __builtin_prefetch(next_packet, 0, 3);
                    }

                    batch_bytes += packet_len;

                    // Replicate the packet to all destinations using the lock-free cache
                    int sent_count = replicatePacket(packet_data, packet_len, queueId);
                    if (__builtin_expect(sent_count > 0, 1)) {
                        batch_sent += sent_count;
                    }
                }

                packets_received_per_queue_[queueId].v.fetch_add((uint64_t)received, std::memory_order_relaxed);
                packets_received_.fetch_add((uint64_t)received, std::memory_order_relaxed);
                bytes_received_.fetch_add(batch_bytes, std::memory_order_relaxed);
                if (batch_sent > 0)
                    packets_sent_per_queue_[queueId].v.fetch_add((uint64_t)batch_sent, std::memory_order_relaxed);
                
                // Recycle the frames back to the fill queue
                xdp_sockets_[queueId]->recycleFrames();
            } else {
                // No frames available; yield the pipeline without a context switch.
                __builtin_ia32_pause();
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
        
    }
    
    std::cout << "HFT-optimized packet processing thread stopped for queue " << queueId << std::endl;
}

// UMEM frame stride. Must match the size XdpSocket registers per frame.
static constexpr uint64_t FRAME_SIZE = 4096;

bool Replicator::processMcastFrame(const uint8_t* m2u_data, size_t m2u_len,
                                    const uint8_t*& payload_data, size_t& payload_len,
                                    uint32_t& group_nbo) {
    // Capture RX time at entry — before parsing — so replicator_ns marks the
    // dequeue instant (AF_XDP ring pop, or recvfrom() return under kernel fwd
    // mode), not the post-parse instant. Keeps the hop1/hop2 split (source->
    // replicator vs replicator->dest) from charging parse cost to hop1.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                + static_cast<uint64_t>(ts.tv_nsec);
    uint64_t replicator_ns_be = __builtin_bswap64(ns);  // to big-endian (x86 is LE)

    // m2u_data must already be positioned at the 8-byte m2u header (magic+group).
    // The AF_XDP path arrives here via extractUdpPayloadMulticast, which already
    // validated the magic while unwrapping Eth/IP/UDP; the kernel fwd mode's
    // recvfrom() buffer starts here directly (the kernel already stripped
    // Eth/IP/UDP), so this magic check is the ONLY validation for that path —
    // not redundant defense-in-depth in that case.
    if (m2u_len < WIRE_M2U_HDR_LEN)
        return false;
    uint32_t magic;
    memcpy(&magic, m2u_data, 4);
    if (ntohl(magic) != WIRE_M2U_MAGIC)
        return false;
    memcpy(&group_nbo, m2u_data + WIRE_M2U_GROUP_OFF, 4);

    payload_data = m2u_data;   // [m2u(8) | app payload]
    payload_len  = m2u_len;

    // Write the entry-captured RX time into the app payload's replicator_ns
    // slot (payload[16..23], i.e. +8 past the m2u header). The sender zeroed
    // the slot; the receiver uses it to split reported latency into hop1
    // (source->replicator) and hop2 (replicator->destination).
    if (payload_len >= WIRE_M2U_HDR_LEN + WIRE_APP_HDR_LEN) {
        memcpy(const_cast<uint8_t*>(payload_data)
                   + WIRE_M2U_HDR_LEN + WIRE_APP_REPL_NS_OFF, &replicator_ns_be, 8);
    }
    return true;
}

int Replicator::replicatePacket(const uint8_t* packetData, size_t packetLen, int queueId) {
    const uint8_t* payload_data = nullptr;
    size_t payload_len = 0;
    uint32_t group_nbo = 0;

    if (!extractUdpPayload(packetData, packetLen, payload_data, payload_len, group_nbo)) {
        return 0; // Not a valid UDP packet
    }

    // mcast mode: extractUdpPayload (via extractUdpPayloadMulticast) already
    // unwrapped Eth/IP/UDP and left payload_data at the m2u header.
    // processMcastFrame does the RX-stamp + m2u parse — shared with
    // processMcastKernelRx (REPLICATOR_FWD_MODE=kernel), which calls it
    // directly on a recvfrom() buffer that has no Eth/IP/UDP to unwrap.
    if (mcast_mode_) {
        if (!processMcastFrame(payload_data, payload_len, payload_data, payload_len, group_nbo))
            return 0;
    }

    // Per-group fan-out: only send to destinations that joined this multicast group via IGMP.
    // Const ref to thread-local per-group cache — no copy, no lock on hot path.
    const std::vector<Destination>& current_destinations = getCachedGroupDestinations(group_nbo);
    if (current_destinations.empty()) {
        return 0;
    }

    int sent_count = 0;
    XdpSocket* sock = (queueId >= 0 && queueId < num_queues_) ? xdp_sockets_[queueId].get() : nullptr;
    if (!sock) {
        for (const Destination& dest : current_destinations)
            if (sendToDestinationFallback(dest, payload_data, payload_len)) sent_count++;
        return sent_count;
    }

    // Drain TX completions once per batch (frees ring slots for all K destinations)
    // rather than once per destination.
    sock->pollTxCompletions();

    const size_t ndest = current_destinations.size();
    size_t first_copy = 0;

    // In-place zero-copy forward (REPLICATOR_FWD_MODE=inplace). The RX frame can be
    // transmitted once, and it goes to destination 0 so the zero-copy path serves the
    // FIRST destination out rather than the one that already waited behind every copy.
    // The payload is stashed first because submitting the frame hands it to the NIC.
    alignas(64) uint8_t stash[FRAME_SIZE];
    const uint8_t* copy_src = payload_data;
    if (fwd_mode_ == 1 && ndest > 0 && payload_len <= sizeof(stash)) {
        memcpy(stash, payload_data, payload_len);
        const uint64_t rx_addr = static_cast<uint64_t>(packetData - sock->getUmemBuffer());
        if (patchHeadersInPlace(current_destinations[0], const_cast<uint8_t*>(packetData), packetLen)
            && sock->forwardFrameInPlace(rx_addr, static_cast<uint32_t>(packetLen))) {
            sent_count++;
            first_copy = 1;
            copy_src = stash;   // RX frame now belongs to TX
        }
    }

    if (first_copy < ndest) {
        // Frame size depends only on payload_len, so validate once for the whole
        // batch. This keeps the reserve/submit counts exactly equal below.
        const size_t frame_len = WIRE_ETH_LEN + WIRE_IP_LEN + WIRE_UDP_LEN + payload_len;
        if (frame_len > FRAME_SIZE) {
            for (size_t di = first_copy; di < ndest; di++)
                if (sendToDestinationFallback(current_destinations[di], copy_src, payload_len)) sent_count++;
        } else {
            // Destinations whose ARP has not resolved carry a broadcast MAC, which
            // ENA/VPC drops; they go through the kernel socket and must NOT consume a
            // reserved TX slot. Count the eligible ones so reserve == submit exactly:
            // a mismatch permanently desyncs the ring's cached and real producer.
            static constexpr uint8_t BROADCAST_MAC[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            size_t nxdp = 0;
            for (size_t di = first_copy; di < ndest; di++)
                if (memcmp(current_destinations[di].mac, BROADCAST_MAC, ETH_ALEN) != 0) nxdp++;

            // One CLOCK_REALTIME read for the batch instead of one per destination:
            // the copies are submitted together, so they share a TX instant.
            uint64_t tx_ns_be = 0;
            if (mcast_mode_) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                tx_ns_be = __builtin_bswap64(static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                                             + static_cast<uint64_t>(ts.tv_nsec));
            }
            // Invariant part of the IPv4 checksum, computed once; each copy folds in
            // only its own daddr rather than re-summing the whole header.
            const uint32_t csum_base = ipCsumInvariantBase(payload_len);

            // One TX slot at a time, as before. Reserving K slots up front was
            // tried and reverted: it produced frames the destinations never
            // received (see the audit notes), and the win was not worth shipping
            // an unverified fan-out. The per-batch completion drain, single driver
            // kick, single clock read and shared checksum base all still apply.
            (void)nxdp;
            static constexpr uint64_t TX_FRAMES_MASK = XdpSocket::DEFAULT_TX_FRAMES - 1;
            for (size_t di = first_copy; di < ndest; di++) {
                const Destination& dest = current_destinations[di];
                if (memcmp(dest.mac, BROADCAST_MAC, ETH_ALEN) == 0) {
                    if (sendToDestinationFallback(dest, copy_src, payload_len)) sent_count++;
                    continue;
                }
                uint32_t tx_idx = 0;
                if (sock->reserveTxRing(1, &tx_idx) != 1) {
                    sock->requestDriverPoll();
                    sock->pollTxCompletions();
                    if (sock->reserveTxRing(1, &tx_idx) != 1) {
                        if (sendToDestinationFallback(dest, copy_src, payload_len)) sent_count++;
                        continue;
                    }
                }
                const uint64_t addr = (static_cast<uint64_t>(tx_idx) & TX_FRAMES_MASK) * FRAME_SIZE;
                uint8_t* buf = sock->getUmemBuffer() + addr;
                const size_t len = buildCopyFrame(dest, copy_src, payload_len, buf, FRAME_SIZE,
                                                  csum_base, tx_ns_be);
                if (len == 0) continue;
                sock->setTxDescriptor(tx_idx, addr, len);
                sock->submitTxRing(1);
                sent_count++;
            }
        }
    }

    // Fold the per-destination counter updates into ONE relaxed RMW each.
    if (sent_count > 0) {
        packets_sent_.fetch_add(static_cast<uint64_t>(sent_count), std::memory_order_relaxed);
        bytes_sent_.fetch_add(static_cast<uint64_t>(sent_count) * payload_len,
                              std::memory_order_relaxed);
    }

    // One driver kick once every destination is queued.
    if (sent_count > 0)
        sock->requestDriverPoll();

    return sent_count;
}

bool Replicator::extractUdpPayloadMulticast(const uint8_t* packetData, size_t packetLen,
                                             const uint8_t*& payloadData, size_t& payloadLen,
                                             uint32_t& group_nbo) {
    // Light m2u tunnel decap: Eth(14) + IPv4 + UDP(8) + m2u(8) + payload.
    // (Historical name kept; the wire format is the flat 8-byte m2u header.)
    // payloadData is set to the m2u header start so the fan-out path can
    // re-emit [m2u | app-payload] verbatim via createUdpPacket().
    static constexpr size_t   M2U_HDR   = WIRE_M2U_HDR_LEN;   // magic(4) + group(4)
    static constexpr uint32_t M2U_MAGIC = WIRE_M2U_MAGIC;     // "M2CU"
    static constexpr size_t   MIN_PKT   = WIRE_PAYLOAD_OFF;
    if (packetLen < MIN_PKT)
        return false;

    const struct ethhdr* eth = reinterpret_cast<const struct ethhdr*>(packetData);
    if (ntohs(eth->h_proto) != ETH_P_IP)
        return false;

    const struct iphdr* ip = reinterpret_cast<const struct iphdr*>(
        packetData + sizeof(struct ethhdr));
    if (ip->protocol != IPPROTO_UDP)
        return false;
    size_t ip_len = static_cast<size_t>(ip->ihl) * 4;
    if (ip_len < 20 || ip_len > 60)
        return false;

    size_t udp_off = sizeof(struct ethhdr) + ip_len;
    if (packetLen < udp_off + sizeof(struct udphdr) + M2U_HDR)
        return false;
    const struct udphdr* udp = reinterpret_cast<const struct udphdr*>(packetData + udp_off);

    size_t hdr_end = udp_off + sizeof(struct udphdr);
    const uint8_t* m2u = packetData + hdr_end;
    uint32_t magic;
    memcpy(&magic, m2u, 4);
    if (ntohl(magic) != M2U_MAGIC)
        return false;
    memcpy(&group_nbo, m2u + WIRE_M2U_GROUP_OFF, 4);   // network byte order multicast group

    // UDP-declared payload length (m2u + app), clamped to the received frame.
    size_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(struct udphdr) + M2U_HDR)
        return false;
    size_t udp_payload = udp_len - sizeof(struct udphdr);
    if (hdr_end + udp_payload > packetLen)
        udp_payload = packetLen - hdr_end;

    payloadData = m2u;            // [m2u(8) | app payload]
    payloadLen  = udp_payload;    // includes the 8-byte m2u header
    return true;
}

bool Replicator::extractUdpPayload(const uint8_t* packetData, size_t packetLen,
                                         const uint8_t*& payloadData, size_t& payloadLen,
                                         uint32_t& group_nbo) {
    if (mcast_mode_)
        return extractUdpPayloadMulticast(packetData, packetLen, payloadData, payloadLen, group_nbo);

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

bool Replicator::sendToDestinationFallback(const Destination& destination, const uint8_t* data, size_t length) {
    // Plain unicast UDP for both modes. In m2u mcast mode `data` is the UDP
    // payload [m2u(8) | app-payload]; in ucast mode it is the app payload.
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

void Replicator::processMcastKernelRx() {
    std::cout << "kernel fwd-mode RX thread started on port " << listen_port_ << std::endl;

    // Socket tuning: apply the AF_XDP RX path's existing busy-poll parity
    // (see XdpSocket.cpp's openSocket — same constants) plus the two options
    // genuinely new to this mode (SO_RCVBUF, SO_TIMESTAMPING). RLIMIT_MEMLOCK
    // and CPU pinning are process-wide/thread-wide and already applied
    // elsewhere (XdpSocket::setResourceLimits() at startup; setCpuAffinity()
    // by the caller of this thread) — not repeated here.
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif
    {
        int on = 1, busy_us = 50, budget = 64, rcvbuf = 4 * 1024 * 1024;
        setsockopt(kernel_rx_socket_, SOL_SOCKET, SO_PREFER_BUSY_POLL, &on, sizeof(on));
        setsockopt(kernel_rx_socket_, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
        setsockopt(kernel_rx_socket_, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget));
        setsockopt(kernel_rx_socket_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        // Kernel-software RX timestamp (SOF_TIMESTAMPING_RX_SOFTWARE): stamped in
        // the NAPI netif_receive_skb path, before socket-queue enqueue — closer to
        // wire time than a clock_gettime() call after recvfrom() returns, which
        // would also fold in socket-queue + wake latency. See rtt.cpp's
        // detect_timestamp_mode for the same rationale applied to the rtt tool.
        int ts_flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        setsockopt(kernel_rx_socket_, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags, sizeof(ts_flags));
        setsockopt(kernel_rx_socket_, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on));
    }

    std::vector<uint8_t> buf(65535);
    struct pollfd pfd{};
    pfd.fd = kernel_rx_socket_;
    pfd.events = POLLIN;

    while (running_.load(std::memory_order_relaxed)) {
        // 500ms timeout, matching KernelEcho.cpp's shutdown pattern: a bare
        // blocking recvfrom() would hang stop() waiting for a packet that may
        // never arrive.
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            if (running_.load(std::memory_order_relaxed))
                std::cerr << "kernel fwd-mode poll error: " << strerror(errno) << std::endl;
            continue;
        }
        if (ret == 0 || !(pfd.revents & POLLIN))
            continue;

        ssize_t n = recvfrom(kernel_rx_socket_, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && running_.load(std::memory_order_relaxed))
                std::cerr << "kernel fwd-mode recvfrom error: " << strerror(errno) << std::endl;
            continue;
        }

        // recvfrom()'s buffer starts at the m2u header — the kernel already
        // stripped Eth/IP/UDP, so processMcastFrame is called directly with
        // no extractUdpPayloadMulticast unwrap (there is nothing to unwrap).
        const uint8_t* payload_data = nullptr;
        size_t payload_len = 0;
        uint32_t group_nbo = 0;
        if (!processMcastFrame(buf.data(), static_cast<size_t>(n), payload_data, payload_len, group_nbo))
            continue;

        const std::vector<Destination>& current_destinations = getCachedGroupDestinations(group_nbo);
        if (current_destinations.empty())
            continue;

        int sent_count = 0;
        for (const Destination& dest : current_destinations) {
            if (sendToDestinationFallback(dest, payload_data, payload_len))
                sent_count++;
        }

        packets_received_.fetch_add(1, std::memory_order_relaxed);
        bytes_received_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        if (sent_count > 0) {
            packets_sent_.fetch_add(static_cast<uint64_t>(sent_count), std::memory_order_relaxed);
            bytes_sent_.fetch_add(static_cast<uint64_t>(sent_count) * payload_len, std::memory_order_relaxed);
        }
    }

    std::cout << "kernel fwd-mode RX thread stopped" << std::endl;
}

bool Replicator::sendToDestinationWithQueue(const Destination& destination, const uint8_t* data, size_t length, int queueId) {
    // If ARP has not yet resolved for this destination, the cached MAC is all-broadcast.
    // ENA/VPC drops frames with broadcast dst MAC, so route through the kernel socket
    // which handles ARP internally. The refresher thread re-resolves the MAC on
    // the next snapshot and automatically restores the AF_XDP fast path.
    static constexpr uint8_t BROADCAST_MAC[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (__builtin_expect(memcmp(destination.mac, BROADCAST_MAC, ETH_ALEN) == 0, 0)) {
        return sendToDestinationFallback(destination, data, length);
    }

    if (queueId < 0 || queueId >= num_queues_ || !xdp_sockets_[queueId]) {
        return sendToDestinationFallback(destination, data, length);
    }

    try {
        // Returns false when the TX ring is full after a retry (P1: this used to
        // throw on the hot path). Fall back to the kernel socket rather than
        // dropping the packet.
        if (sendSinglePacketDirect(destination, data, length, queueId))
            return true;
        return sendToDestinationFallback(destination, data, length);
    } catch (const std::exception& e) {
        std::cerr << "Direct AF_XDP send failed on queue " << queueId << ": " << e.what()
                  << ", falling back to regular socket" << std::endl;
        return sendToDestinationFallback(destination, data, length);
    }
}

bool Replicator::sendSinglePacketDirect(const Destination& destination, const uint8_t* data, size_t length, int queueId) {
    // Direct single packet transmission following ena-xdp exactly (no batching)
    XdpSocket* xdp_socket = xdp_sockets_[queueId].get();
    
    DEBUG_TX_PRINT("DEBUG TX: Starting TX for " << destination.ip_address << ":" << destination.port 
              << ", data_len=" << length << ", queue=" << queueId);
    
    // Drain completions is done once per batch in replicatePacket(); the retry
    // path below still polls if this destination happens to hit a full ring.
    // (was: xdp_socket->pollTxCompletions() here, once per destination)

    // Reserve a TX ring slot first; derive the UMEM frame address from the ring index.
    // NOTE (B5): ring-slot availability alone does NOT prove the frame is free — the
    // TX ring's consumer index advances when the kernel dequeues the descriptor, not
    // when DMA completes. Frame safety comes from the outstanding_tx_ < TX_FRAMES
    // guard now enforced inside XdpSocket::reserveTxRing(); see the comment there.
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
            // TX ring full even after the retry. Return a status instead of
            // throwing: this fires exactly when we are LOADED, and an
            // exception throw/unwind (plus the per-packet cerr in the old
            // handler) turned back-pressure into a latency cliff. The caller
            // falls back to the kernel socket.
            DEBUG_TX_PRINT("DEBUG TX: TX ring full after retry, falling back");
            return false;
        }
    }

    // Frame address derived from ring slot — power-of-2 modulo via bitmask
    static constexpr uint64_t TX_FRAMES_MASK = XdpSocket::DEFAULT_TX_FRAMES - 1;
    uint64_t tx_frame_addr = (static_cast<uint64_t>(tx_idx) & TX_FRAMES_MASK) * FRAME_SIZE;

    DEBUG_TX_PRINT("DEBUG TX: tx_idx=" << tx_idx << ", tx_frame_addr=0x"
              << std::hex << tx_frame_addr << std::dec);

    // Build outgoing packet: plain unicast UDP. In m2u mode `data` already
    // begins with the 8-byte m2u header, so the destination receives
    // Eth|IP|UDP|m2u|payload — the same framing mcast.o matches on RX.
    uint8_t* tx_buffer = xdp_socket->getUmemBuffer() + tx_frame_addr;
    size_t packet_len = createUdpPacket(destination, data, length, tx_buffer, FRAME_SIZE);
    if (packet_len == 0) {
        DEBUG_TX_PRINT("DEBUG TX: packet build failed!");
        return false;
    }

    DEBUG_TX_PRINT("DEBUG TX: Created packet, len=" << packet_len);

    // Stamp replicator_tx_ns as late as possible (right before submit) so the
    // receiver can split hop2 into replicator processing (replicator_tx_ns -
    // replicator_ns) vs wire+dest-RX (rx_ns - replicator_tx_ns). Offset within the
    // TX frame: Eth(14)+IP(20)+UDP(8)+m2u(8) + app-payload slot at +24.
    // mcast ONLY: this offset lands inside the m2u app header; in ucast the payload
    // is opaque application data (e.g. rtt's seq at offset 38) and must NOT be mutated.
    static constexpr size_t REPL_TX_OFF = WIRE_REPL_TX_FRAME_OFF;
    if (mcast_mode_ && packet_len >= REPL_TX_OFF + 8) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t txns_be = __builtin_bswap64(
            static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec));
        memcpy(tx_buffer + REPL_TX_OFF, &txns_be, 8);
    }

    // Fill TX descriptor (ena-xdp pattern)
    xdp_socket->setTxDescriptor(tx_idx, tx_frame_addr, packet_len);

    DEBUG_TX_PRINT("DEBUG TX: Set TX descriptor, addr=0x" << std::hex << tx_frame_addr
              << std::dec << ", len=" << packet_len);
    
    // Submit TX ring entry — driver kick is batched at the replicatePacket() call site
    // so the wakeup is issued once for all K destinations rather than K times.
    xdp_socket->submitTxRing(1);

    DEBUG_TX_PRINT("DEBUG TX: Submitted to TX ring");

    return true;
}

size_t Replicator::createUdpPacket(const Destination& destination, const uint8_t* payload, size_t payloadLen,
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
    ip->saddr    = cached_iface_saddr_nbo_;  // parsed once at initialize() — no per-packet inet_aton
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

uint32_t Replicator::ipCsumInvariantBase(size_t payloadLen) const {
    // Sum the IPv4 header words that every copy in a fan-out shares. daddr and
    // check are left zero, so a copy only has to fold in its own daddr. Summing a
    // header laid out in memory is byte-order agnostic (RFC 1071).
    struct iphdr ip;
    memset(&ip, 0, sizeof(ip));
    ip.version  = 4;
    ip.ihl      = WIRE_IP_IHL_NO_OPTIONS;
    ip.tos      = 0;
    ip.tot_len  = htons(static_cast<uint16_t>(WIRE_IP_LEN + WIRE_UDP_LEN + payloadLen));
    ip.id       = 0;
    ip.frag_off = htons(IP_DF);
    ip.ttl      = 64;
    ip.protocol = IPPROTO_UDP;
    ip.check    = 0;
    ip.saddr    = cached_iface_saddr_nbo_;
    ip.daddr    = 0;

    uint32_t sum = 0;
    const uint16_t* w = reinterpret_cast<const uint16_t*>(&ip);
    for (int i = 0; i < WIRE_IP_IHL_WORDS; i++) sum += w[i];
    return sum;
}

size_t Replicator::buildCopyFrame(const Destination& destination, const uint8_t* payload, size_t payloadLen,
                                  uint8_t* buffer, size_t bufferSize,
                                  uint32_t ipCsumBase, uint64_t txNsBe) {
    const size_t total_len = WIRE_ETH_LEN + WIRE_IP_LEN + WIRE_UDP_LEN + payloadLen;
    if (total_len > bufferSize)
        return 0;

    struct ethhdr* eth = reinterpret_cast<struct ethhdr*>(buffer);
    memcpy(eth->h_dest,   destination.mac,  ETH_ALEN);
    memcpy(eth->h_source, cached_iface_mac_, ETH_ALEN);
    eth->h_proto = htons(ETH_P_IP);

    struct iphdr* ip = reinterpret_cast<struct iphdr*>(buffer + WIRE_ETH_LEN);
    ip->version  = 4;
    ip->ihl      = WIRE_IP_IHL_NO_OPTIONS;
    ip->tos      = 0;
    ip->tot_len  = htons(static_cast<uint16_t>(WIRE_IP_LEN + WIRE_UDP_LEN + payloadLen));
    ip->id       = 0;             // Atomic datagram: ID=0 per RFC 6864 when DF is set
    ip->frag_off = htons(IP_DF);
    ip->ttl      = 64;
    ip->protocol = IPPROTO_UDP;
    ip->saddr    = cached_iface_saddr_nbo_;
    ip->daddr    = destination.addr.sin_addr.s_addr;

    // Full recompute over the 20-byte header. A precomputed invariant base was
    // tried and reverted: summing a freshly built local struct through a uint16_t*
    // is a strict-aliasing violation, and under -O3 -flto it yielded a checksum the
    // VPC rejected, so every fan-out copy was dropped with no local counter moving.
    (void)ipCsumBase;
    ip->check = 0;
    uint32_t sum = 0;
    const uint16_t* w = reinterpret_cast<const uint16_t*>(ip);
    for (int i = 0; i < WIRE_IP_IHL_WORDS; i++) sum += w[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->check = static_cast<uint16_t>(~sum);

    struct udphdr* udp = reinterpret_cast<struct udphdr*>(buffer + WIRE_ETH_LEN + WIRE_IP_LEN);
    udp->source = htons(listen_port_);
    udp->dest   = destination.addr.sin_port;
    udp->len    = htons(static_cast<uint16_t>(WIRE_UDP_LEN + payloadLen));
    udp->check  = 0;  // optional for IPv4

    memcpy(buffer + WIRE_ETH_LEN + WIRE_IP_LEN + WIRE_UDP_LEN, payload, payloadLen);

    // replicator_tx_ns from the batch's single clock read. mcast only: the offset
    // lands inside the m2u app header, and a ucast payload is opaque.
    if (mcast_mode_ && txNsBe != 0 && total_len >= WIRE_REPL_TX_FRAME_OFF + 8)
        memcpy(buffer + WIRE_REPL_TX_FRAME_OFF, &txNsBe, 8);

    return total_len;
}

bool Replicator::patchHeadersInPlace(const Destination& destination, uint8_t* frame, size_t frame_len) {
    // Rewrite the RX frame's L2/L3/L4 headers for `destination` — no payload copy.
    // The source sends a standard 20-byte IPv4 header (ihl=5), so offsets are fixed.
    static constexpr size_t ETH = 14, IP = 20, UDP = 8;
    if (frame_len < ETH + IP + UDP) return false;

    struct ethhdr* eth = reinterpret_cast<struct ethhdr*>(frame);
    memcpy(eth->h_dest,   destination.mac,  ETH_ALEN);
    memcpy(eth->h_source, cached_iface_mac_, ETH_ALEN);
    // h_proto already ETH_P_IP (the RX frame is IPv4).

    struct iphdr* ip = reinterpret_cast<struct iphdr*>(frame + ETH);
    // Fast paths recompute the checksum over exactly WIRE_IP_IHL_WORDS (20 bytes).
    // Frames with IP options (ihl > 5) carry more header bytes; refuse to patch them
    // so the caller uses the copy path, which rebuilds headers from scratch.
    if (ip->ihl != WIRE_IP_IHL_NO_OPTIONS) return false;
    ip->saddr = cached_iface_saddr_nbo_;
    ip->daddr = destination.addr.sin_addr.s_addr;
    ip->check = 0;
    uint32_t sum = 0;
    const uint16_t* w = reinterpret_cast<const uint16_t*>(ip);
    for (int i = 0; i < WIRE_IP_IHL_WORDS; i++) sum += w[i];  // 20-byte header (ihl==5 enforced)
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->check = static_cast<uint16_t>(~sum);

    struct udphdr* udp = reinterpret_cast<struct udphdr*>(frame + ETH + IP);
    udp->source = htons(listen_port_);
    udp->dest   = destination.addr.sin_port;
    udp->check  = 0;  // optional for IPv4

    // Stamp replicator_tx_ns in place (app-payload slot at m2u+24 → frame +74).
    // mcast ONLY (see sendSinglePacketDirect): the offset is inside the m2u app
    // header; never mutate an opaque ucast payload.
    static constexpr size_t REPL_TX_OFF = WIRE_REPL_TX_FRAME_OFF;  // 74
    if (mcast_mode_ && frame_len >= REPL_TX_OFF + 8) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t txns_be = __builtin_bswap64(
            static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec));
        memcpy(frame + REPL_TX_OFF, &txns_be, 8);
    }
    return true;
}
