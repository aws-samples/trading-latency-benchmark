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

// HFT OPTIMIZED: Removed processPackets() method - using processPacketsForQueue() instead

void Replicator::processPacketsForQueue(int queueId) {
    std::cout << "HFT-optimized packet processing thread started for queue " << queueId << std::endl;
    
    // HFT OPTIMIZATION: Pre-allocate batch vectors with cache-aligned memory.
    // mcast mode: source sends multi-hundred-frame bursts — use 256 to drain in one peek.
    // Unicast mode: sparse arrivals; 64 is never the limiting factor.
    // 256 fits well within the 2048-frame RX UMEM partition (256 in-flight + 1792 in fill queue).
    const int rx_batch = mcast_mode_ ? 256 : 64;
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

int Replicator::replicatePacket(const uint8_t* packetData, size_t packetLen, int queueId) {
    // Capture RX time at entry — before header parsing — so replicator_ns marks
    // the dequeue instant, not the post-parse instant. Keeps the hop1/hop2 split
    // (source->replicator vs replicator->dest) from charging parse cost to hop1.
    uint64_t replicator_ns_be = 0;
    if (mcast_mode_) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                    + static_cast<uint64_t>(ts.tv_nsec);
        replicator_ns_be = __builtin_bswap64(ns);  // to big-endian (x86 is LE)
    }

    const uint8_t* payload_data = nullptr;
    size_t payload_len = 0;
    uint32_t group_nbo = 0;

    if (!extractUdpPayload(packetData, packetLen, payload_data, payload_len, group_nbo)) {
        return 0; // Not a valid UDP packet
    }

    // m2u mode: write the entry-captured RX time into the app payload's
    // replicator_ns slot (payload[16..23]). payload_data points at the 8-byte
    // m2u header, so the app payload starts at +8. The sender zeroed the slot;
    // the receiver uses it to split reported latency into hop1 (source->replicator)
    // and hop2 (replicator->destination).
    if (mcast_mode_) {
        static constexpr size_t M2U_HDR = 8;
        if (payload_len >= M2U_HDR + 24) {  // m2u + HDR_SIZE
            memcpy(const_cast<uint8_t*>(payload_data) + M2U_HDR + 16, &replicator_ns_be, 8);
        }
    }

    // Per-group fan-out: only send to destinations that joined this multicast group via IGMP.
    // Const ref to thread-local per-group cache — no copy, no lock on hot path.
    const std::vector<Destination>& current_destinations = getCachedGroupDestinations(group_nbo);
    if (current_destinations.empty()) {
        return 0;
    }

    int sent_count = 0;
    // Drain TX completions once per batch (frees ring slots for all K destinations)
    // rather than once per destination inside sendSinglePacketDirect().
    xdp_sockets_[queueId]->pollTxCompletions();
    const size_t ndest = current_destinations.size();
    for (size_t di = 0; di < ndest; di++) {
        const Destination& dest = current_destinations[di];
        // In-place zero-copy forward (REPLICATOR_FWD_MODE=inplace): the RX frame can
        // only be transmitted once, so use it for the LAST destination and copy the
        // rest. Falls back to the copy path if patch/submit fails.
        if (fwd_mode_ == 1 && di + 1 == ndest) {
            uint64_t rx_addr = static_cast<uint64_t>(
                packetData - xdp_sockets_[queueId]->getUmemBuffer());
            if (patchHeadersInPlace(dest, const_cast<uint8_t*>(packetData), packetLen)
                && xdp_sockets_[queueId]->forwardFrameInPlace(rx_addr, static_cast<uint32_t>(packetLen))) {
                sent_count++;
                packets_sent_++;
                bytes_sent_ += payload_len;
                continue;
            }
        }
        if (sendToDestinationWithQueue(dest, payload_data, payload_len, queueId)) {
            sent_count++;
            packets_sent_++;
            bytes_sent_ += payload_len;
        }
    }

    // One driver kick after all K destinations have been queued — avoids K-1 redundant
    // needs_wakeup checks and potential sendto syscalls inside the per-destination loop.
    if (sent_count > 0)
        xdp_sockets_[queueId]->requestDriverPoll();

    return sent_count;
}

bool Replicator::extractUdpPayloadMulticast(const uint8_t* packetData, size_t packetLen,
                                             const uint8_t*& payloadData, size_t& payloadLen,
                                             uint32_t& group_nbo) {
    // Light m2u tunnel decap: Eth(14) + IPv4 + UDP(8) + m2u(8) + payload.
    // (Historical name kept; the wire format is the flat 8-byte m2u header.)
    // payloadData is set to the m2u header start so the fan-out path can
    // re-emit [m2u | app-payload] verbatim via createUdpPacket().
    static constexpr size_t   M2U_HDR   = 8;            // magic(4) + group(4)
    static constexpr uint32_t M2U_MAGIC = 0x4D324355;   // "M2CU"
    static constexpr size_t   MIN_PKT   = 14 + 20 + 8 + M2U_HDR;
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
    memcpy(&group_nbo, m2u + 4, 4);   // network byte order multicast group

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

bool Replicator::sendToDestinationWithQueue(const Destination& destination, const uint8_t* data, size_t length, int queueId) {
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

bool Replicator::sendSinglePacketDirect(const Destination& destination, const uint8_t* data, size_t length, int queueId) {
    // Direct single packet transmission following ena-xdp exactly (no batching)
    XdpSocket* xdp_socket = xdp_sockets_[queueId].get();
    
    DEBUG_TX_PRINT("DEBUG TX: Starting TX for " << destination.ip_address << ":" << destination.port 
              << ", data_len=" << length << ", queue=" << queueId);
    
    // Drain completions is done once per batch in replicatePacket(); the retry
    // path below still polls if this destination happens to hit a full ring.
    // (was: xdp_socket->pollTxCompletions() here, once per destination)

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
    static constexpr size_t REPL_TX_OFF = 14 + 20 + 8 + 8 + 24;
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
    ip->saddr = cached_iface_saddr_nbo_;
    ip->daddr = destination.addr.sin_addr.s_addr;
    ip->check = 0;
    uint32_t sum = 0;
    const uint16_t* w = reinterpret_cast<const uint16_t*>(ip);
    for (int i = 0; i < 10; i++) sum += w[i];   // 20-byte header
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->check = static_cast<uint16_t>(~sum);

    struct udphdr* udp = reinterpret_cast<struct udphdr*>(frame + ETH + IP);
    udp->source = htons(listen_port_);
    udp->dest   = destination.addr.sin_port;
    udp->check  = 0;  // optional for IPv4

    // Stamp replicator_tx_ns in place (app-payload slot at m2u+24 → frame +74).
    // mcast ONLY (see sendSinglePacketDirect): the offset is inside the m2u app
    // header; never mutate an opaque ucast payload.
    static constexpr size_t REPL_TX_OFF = ETH + IP + UDP + 8 + 24;  // 74
    if (mcast_mode_ && frame_len >= REPL_TX_OFF + 8) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t txns_be = __builtin_bswap64(
            static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec));
        memcpy(frame + REPL_TX_OFF, &txns_be, 8);
    }
    return true;
}
