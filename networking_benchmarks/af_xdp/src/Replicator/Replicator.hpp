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

#ifndef PACKET_REPLICATOR_HPP
#define PACKET_REPLICATOR_HPP

#include "XdpSocket.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>
#include <set>
#include <array>
#include <unordered_map>
#include <netinet/in.h>
#include <cstdint>
#include <chrono>
#include <immintrin.h>  // For CPU pause instruction
#include <sched.h>      // For CPU affinity

/**
 * High-performance packet replicator using AF_XDP zero copy
 * 
 * This replicator:
 * 1. Listens for incoming UDP packets to a specific IP and port
 * 2. Uses AF_XDP zero copy to receive packets with minimal latency
 * 3. Replicates received packets to multiple destination EC2 instances
 * 4. Provides control protocol for managing destination instances
 */
class Replicator {
public:
    // Control protocol constants
    static constexpr int CONTROL_PORT = 12345;
    static constexpr uint8_t CTRL_ADD_DESTINATION    = 1;
    static constexpr uint8_t CTRL_REMOVE_DESTINATION = 2;
    static constexpr uint8_t CTRL_LIST_DESTINATIONS  = 3;
    // Per-group subscription: destination specifies which group + port to receive on.
    // Replicator infers destination IP from the UDP source address of the control message.
    static constexpr uint8_t CTRL_MCAST_JOIN  = 4;  // [4][4B group]
    static constexpr uint8_t CTRL_MCAST_LEAVE = 5;  // [5][4B group]

    // Destination instance information
    struct Destination {
        std::string ip_address;
        uint16_t port;
        struct sockaddr_in addr;
        uint8_t mac[6];  // Destination MAC resolved via ARP at addDestination() time

        Destination(const std::string& ip, uint16_t p);
        bool operator<(const Destination& other) const;
    };

private:
    std::string listen_interface_;
    std::string listen_ip_;
    uint16_t listen_port_;

    // Interface IP and MAC cached once at initialize() — never re-queried on hot path
    std::string cached_iface_ip_;
    uint32_t    cached_iface_saddr_nbo_ = 0;  // parsed once at initialize(); createUdpPacket() hot path avoids per-packet inet_aton
    uint8_t     cached_iface_mac_[6]{};
    int num_queues_;
    bool mcast_mode_;         // mcast mode: m2u-tagged unicast UDP carries the multicast group
    // Forward path (REPLICATOR_FWD_MODE env): 0=copy (build packet in a TX-pool frame),
    // 1=inplace (patch the RX frame's headers + TX that same UMEM frame — no payload copy),
    // 2=bpf_tx (XDP program forwards via XDP_TX; userspace fan-out is bypassed),
    // 3=kernel (plain UDP sockets end-to-end; no XDP/eBPF anywhere in the mcast path).
    int  fwd_mode_ = 0;

    // kernel fwd mode's RX socket (fwd_mode_==3). Bound once in initialize();
    // processMcastKernelRx() polls it. -1 for every other fwd mode.
    int kernel_rx_socket_{-1};

    // kernel fwd mode only changes behavior in mcast mode: unicast delivery has
    // no equivalent to CTRL_MCAST_JOIN's destination registration, so a plain
    // kernel RX loop would have no delivery mechanism. See initialize()'s notice
    // logged when this is false but REPLICATOR_FWD_MODE=kernel was requested.
    bool kernelFwdActive() const { return fwd_mode_ == 3 && mcast_mode_; }

    // ── Dynamic group tracking (mcast mode) / static seed (unicast mode) ─────
    // config_map_fd_: BPF map fd retained after initialize() for runtime updates.
    int config_map_fd_{-1};
    int fwd_map_fd_{-1};   // in-kernel XDP_TX forward targets (REPLICATOR_FWD_MODE=bpf_tx)

    // Per-group BPF state.  All maps keyed by group IP in network byte order,
    // protected by group_mutex_.  Used by mcast mode only.
    std::unordered_map<uint32_t, uint32_t> group_slots_;        // group NBO → config_map slot index
    std::unordered_map<uint32_t, int>      group_ref_counts_;   // group NBO → destination join count
    std::vector<uint32_t>                  free_slots_;         // available config_map slot indices
    std::mutex                             group_mutex_;

    // Per-group destination destinations.  Protected by destinations_mutex_.
    // Maps group NBO → (destination IP string → Destination with port + ARP-resolved MAC).
    // Populated by CTRL_MCAST_JOIN (mcast mode).
    std::unordered_map<uint32_t, std::unordered_map<std::string, Destination>> group_destinations_;

    // listen_ip_ parsed to NBO once at initialize(); used as cache key in unicast mode.
    uint32_t listen_ip_nbo_{0};

    // Upstream control: destination multicast → replicator → producer forwarding
    std::string ctrl_multicast_group_;  // Multicast group destinations send control messages to
    uint16_t    ctrl_multicast_port_{0};
    std::string producer_ip_;           // Unicast IP of upstream producer to forward control to
    uint16_t    producer_port_{0};
    int ctrl_multicast_socket_{-1};     // Receives control multicast; holds IGMP membership
    int ctrl_forward_socket_{-1};       // Sends forwarded control messages to producer
    
    std::vector<std::unique_ptr<XdpSocket>> xdp_sockets_;
    int control_socket_;
    int output_socket_;  // Fallback regular socket
    
    std::atomic<bool> running_;
    std::vector<std::unique_ptr<std::thread>> packet_processor_threads_;
    std::unique_ptr<std::thread> control_thread_;
    std::unique_ptr<std::thread> ctrl_upstream_thread_;
    
    mutable std::mutex destinations_mutex_;
    // Canonical destination registry: IP string → Destination (with ARP-resolved MAC).
    // Protected by destinations_mutex_.
    std::unordered_map<std::string, Destination> all_destinations_;
    
    // Immutable fan-out snapshot, published by the refresher thread and read by
    // the packet threads. The hot path only does one acquire load: no clock read,
    // no mutex, and no ARP resolution, all of which used to run inline on the RX
    // thread every 100 ms and stalled it.
    struct DestSnapshot {
        // Maps multicast group NBO → destinations interested in that group.
        std::unordered_map<uint32_t, std::vector<Destination>> group_dests;
    };
    // Retired snapshots stay alive for RETAIN generations before their slot is
    // reused. A reader holds a snapshot pointer only for the duration of one
    // packet, so RETAIN x 100 ms is an ample grace period and lets readers run
    // without any reference counting.
    static constexpr size_t SNAPSHOT_RETAIN = 8;
    std::array<std::shared_ptr<DestSnapshot>, SNAPSHOT_RETAIN> snapshot_ring_;
    size_t snapshot_ring_pos_{0};
    std::atomic<const DestSnapshot*> dest_snapshot_{nullptr};
    std::unique_ptr<std::thread> dest_refresh_thread_;
    static constexpr std::chrono::milliseconds DEST_REFRESH_INTERVAL{100};
    // Per-thread memo of the last {snapshot, group} lookup, so a steady stream
    // keyed by one group skips the hash lookup entirely.
    static thread_local const DestSnapshot* tls_memo_snap_;
    static thread_local uint32_t tls_memo_group_;
    static thread_local const std::vector<Destination>* tls_memo_vec_;
    
    // CPU cores assigned to packet-processing threads.
    std::vector<int> cpu_cores_;
    bool enable_cpu_affinity_{true};
    
    // Statistics (per-queue and total) - Cache aligned for performance
    static constexpr int MAX_QUEUES = 8;  // Support up to 8 queues
    // Each per-queue counter occupies its own cache line so queue threads do not
    // invalidate each other's counters. Necessary for correct scaling when RSS delivers
    // frames across multiple queues on different cores.
    struct alignas(64) PaddedCounter {
        std::atomic<uint64_t> v{0};
    };
    alignas(64) std::array<PaddedCounter, MAX_QUEUES> packets_received_per_queue_;
    alignas(64) std::array<PaddedCounter, MAX_QUEUES> packets_sent_per_queue_;
    alignas(64) std::atomic<uint64_t> packets_received_;
    alignas(64) std::atomic<uint64_t> packets_sent_;
    alignas(64) std::atomic<uint64_t> bytes_received_;
    alignas(64) std::atomic<uint64_t> bytes_sent_;

public:
    /**
     * Creates a new Replicator
     * 
     * @param interface Network interface to bind to (e.g., "eth0")
     * @param listenIp  IP address to listen on
     * @param listenPort Port to listen on
     * @throws std::runtime_error If initialization fails
     */
    Replicator(const std::string& interface, const std::string& listenIp, uint16_t listenPort, int numQueues = 4);

    /**
     * Destructor
     */
    ~Replicator();

    // Copy constructor and assignment operator are deleted
    Replicator(const Replicator&) = delete;
    Replicator& operator=(const Replicator&) = delete;

    // Move constructor and assignment operator
    Replicator(Replicator&& other) noexcept;
    Replicator& operator=(Replicator&& other) noexcept;

    /**
     * Initialize AF_XDP socket and XDP program
     *
     * @param useZeroCopy Whether to use zero-copy mode (requires driver support)
     * @throws std::runtime_error If initialization fails
     */
    void initialize(bool useZeroCopy = true);

    /**
     * Enable multicast (m2u) mode.
     * Must be called before initialize().
     * In mcast mode: mcast.o is loaded; the m2u-tagged unicast UDP frame arrives on
     * eth0 (preserving XDP_ZEROCOPY on ENA) and Replicator strips the 8-byte m2u
     * headers in userspace.  listen_ip_ still holds the inner multicast group
     * address used for config_map; no IGMP join is performed.
     */
    void setMcastMode(bool enable) { mcast_mode_ = enable; }

    /**
     * Configure upstream control forwarding.
     * Destinations send control messages to ctrlGroup:ctrlPort (multicast).
     * The replicator receives them and forwards to producerIp:producerPort (unicast).
     * Must be called before initialize().
     */
    void setUpstreamControl(const std::string& ctrlGroup, uint16_t ctrlPort,
                            const std::string& producerIp, uint16_t producerPort);

    /**
     * Add a destination EC2 instance
     * 
     * @param ipAddress IP address of the destination
     * @param port      Port of the destination
     * @throws std::runtime_error If adding destination fails
     */
    void addDestination(const std::string& ipAddress, uint16_t port);

    /**
     * Remove a destination EC2 instance
     * 
     * @param ipAddress IP address of the destination
     * @param port      Port of the destination
     * @throws std::runtime_error If removing destination fails
     */
    void removeDestination(const std::string& ipAddress, uint16_t port);

    /**
     * Get list of current destinations
     * 
     * @return Vector of current destinations
     */
    std::vector<Destination> getDestinations() const;

    /**
     * Start the packet replicator
     * This will start packet processing and control protocol handling
     */
    void start();

    /**
     * Stop the packet replicator
     */
    void stop();

    /**
     * Check if the replicator is running
     * 
     * @return True if running, false otherwise
     */
    bool isRunning() const;

    /**
     * Get statistics
     */
    struct Statistics {
        uint64_t packets_received;
        uint64_t packets_sent;
        uint64_t bytes_received;
        uint64_t bytes_sent;
        size_t destinations_count;
    };

    Statistics getStatistics() const;

    /**
     * Print current statistics
     */
    void printStatistics() const;

private:
    /**
     * Configure the XDP program: stores config_map_fd_, zeroes all slots,
     * populates free_slots_.  In mcast mode writes listen_ip_/listen_port_ to
     * slot 0 immediately (static inner group).
     */
    void configureXdpProgram();

    /**
     * Dynamically add a group to the BPF config_map (mcast mode).
     * Thread-safe; called from processControlMessage on CTRL_MCAST_JOIN.
     * @param group_nbo  Multicast group address in network byte order.
     */
    void addGroupDynamic(uint32_t group_nbo);

    /**
     * Dynamically remove a group (ref-count semantics).  Zeroes the config_map slot.
     * @param group_nbo  Multicast group address in network byte order.
     */
    void removeGroupDynamic(uint32_t group_nbo);

    /**
     * Join the control multicast group so the replicator receives destination control messages.
     */
    void joinControlMulticastGroup();

    /**
     * Receive loop: reads from ctrl_multicast_socket_ and forwards each datagram
     * verbatim to producer_ip_:producer_port_ via ctrl_forward_socket_.
     */
    void handleUpstreamControl();

    /**
     * Packet processing loop for a specific queue
     * 
     * @param queueId Queue ID to process packets for
     */
    void processPacketsForQueue(int queueId);

    /**
     * REPLICATOR_FWD_MODE=kernel RX loop: one thread, one plain UDP socket,
     * no AF_XDP/eBPF anywhere in the path. poll()-with-timeout so stop() is
     * never blocked on a recvfrom() that may never return. Mirrors
     * processPacketsForQueue's role but for the kernel-socket transport.
     */
    void processMcastKernelRx();

    /**
     * Handle control protocol messages
     */
    void handleControlProtocol();

    /**
     * Replicate a single packet to all destinations
     * 
     * @param packetData Pointer to packet data in UMEM
     * @param packetLen  Length of the packet
     * @param queueId    Queue ID for the socket to use
     * @return Number of destinations the packet was sent to
     */
    int replicatePacket(const uint8_t* packetData, size_t packetLen, int queueId);

    /**
     * Extract UDP payload from a packet (dispatches to m2u or plain path).
     * Also returns the multicast group NBO address via group_nbo (used for per-group fan-out).
     * Unicast: group_nbo = outer IP daddr (the multicast group the source sent to).
     * mcast:   group_nbo = the group read from the 8-byte m2u tunnel header.
     */
    bool extractUdpPayload(const uint8_t* packetData, size_t packetLen,
                          const uint8_t*& payloadData, size_t& payloadLen,
                          uint32_t& group_nbo);

    /**
     * m2u payload extraction.
     * Strips: Eth + IPv4 + UDP + 8-byte m2u header; returns payload at the m2u header.
     * Returns inner IP datagram (IPv4+UDP+payload) verbatim in payloadData/payloadLen.
     * group_nbo receives the inner IP destination (multicast group).
     */
    bool extractUdpPayloadMulticast(const uint8_t* packetData, size_t packetLen,
                              const uint8_t*& payloadData, size_t& payloadLen,
                              uint32_t& group_nbo);

    /**
     * Transport-agnostic m2u frame processing: reads the m2u magic/group at
     * the start of m2u_data and stamps replicator_ns into the app payload.
     * Does NOT unwrap Eth/IP/UDP — the caller must already have positioned
     * m2u_data at the m2u header (AF_XDP: after extractUdpPayloadMulticast;
     * kernel socket: recvfrom()'s buffer already starts here, the kernel
     * stripped Eth/IP/UDP before userspace ever sees it).
     * Shared by replicatePacket (AF_XDP) and processMcastKernelRx (kernel fwd mode).
     *
     * @param m2u_data    Buffer starting at the 8-byte m2u header.
     * @param m2u_len     Length of m2u_data.
     * @param payload_data Output: same as m2u_data (payload is emitted [m2u|app] verbatim).
     * @param payload_len  Output: length of the m2u+app payload, clamped to m2u_len.
     * @param group_nbo    Output: multicast group, network byte order.
     * @return false if m2u_data is too short or the magic doesn't match.
     */
    bool processMcastFrame(const uint8_t* m2u_data, size_t m2u_len,
                            const uint8_t*& payload_data, size_t& payload_len,
                            uint32_t& group_nbo);

    /**
     * Fallback method using regular socket when zero-copy fails
     * 
     * @param destination Target destination
     * @param data        Data to send
     * @param length      Length of data
     * @return True if sent successfully
     */
    bool sendToDestinationFallback(const Destination& destination, const uint8_t* data, size_t length);

    /**
     * Send UDP packet to destination using specific queue's socket
     * 
     * @param destination Target destination
     * @param data        Data to send
     * @param length      Length of data
     * @param queueId     Queue ID to use for transmission
     * @return True if sent successfully
     */
    bool sendToDestinationWithQueue(const Destination& destination, const uint8_t* data, size_t length, int queueId);

    /**
     * Send single packet directly (ena-xdp approach, no batching)
     * 
     * @param destination Target destination
     * @param data        Data to send
     * @param length      Length of data
     * @param queueId     Queue ID to use for transmission
     * @return True if sent successfully
     */
    bool sendSinglePacketDirect(const Destination& destination, const uint8_t* data, size_t length, int queueId);

    /**
     * Create UDP packet with headers for zero-copy transmission (plain unicast; carries m2u payload in mcast mode)
     */
    size_t createUdpPacket(const Destination& destination, const uint8_t* payload, size_t payloadLen,
                          uint8_t* buffer, size_t bufferSize);

    /**
     * In-place forward header patch (REPLICATOR_FWD_MODE=inplace): rewrite the
     * RX frame's Eth/IP/UDP for `destination` (dst+src MAC/IP, UDP dst port, IP
     * checksum) and stamp replicator_tx_ns — no payload copy. `frame` points at
     * the start of the RX frame (Ethernet header); `frame_len` is its length.
     * Returns true if the frame was long enough to patch.
     */
    bool patchHeadersInPlace(const Destination& destination, uint8_t* frame, size_t frame_len);

    // Fan-out snapshot publisher: rebuilds off the RX thread and publishes.
    void destRefreshLoop();
    void publishDestSnapshot();

    // Sum of the invariant IPv4 header words for this payload size, with daddr
    // and check as zero. Each copy then folds in only its own daddr.
    uint32_t ipCsumInvariantBase(size_t payloadLen) const;

    // Build one fan-out copy into `buffer`, reusing the batch's checksum base and
    // TX timestamp. Returns the frame length, or 0 if it would not fit.
    size_t buildCopyFrame(const Destination& destination, const uint8_t* payload, size_t payloadLen,
                          uint8_t* buffer, size_t bufferSize,
                          uint32_t ipCsumBase, uint64_t txNsBe);

    /**
     * Populate/clear the in-kernel XDP_TX forward target (REPLICATOR_FWD_MODE=bpf_tx)
     * for the config_map slot of `group_nbo`, so mcast.o forwards this group's
     * frames to `dest` entirely in the kernel. No-op unless fwd_mode_ == bpf_tx.
     */
    void updateBpfTxFwdTarget(uint32_t group_nbo, const Destination& dest, bool enable);


    /**
     * Process control message
     * 
     * @param message    Control message data
     * @param messageLen Length of control message
     * @param clientAddr Address of client that sent the message
     * @return Response message (empty if no response needed)
     */
    std::vector<uint8_t> processControlMessage(const uint8_t* message, size_t messageLen, 
                                               const struct sockaddr_in& clientAddr);

    /**
     * Helper method to parse IP address string to network byte order
     */
    uint32_t parseIpAddress(const std::string& ipStr);

    /**
     * Helper method to format IP address from network byte order to string
     */
    std::string formatIpAddress(uint32_t ipAddr);

    /**
     * Get interface IP address
     * 
     * @param interface Interface name
     * @param ip_address Output IP address string
     * @return True if successful
     */
    bool getInterfaceIp(const std::string& interface, std::string& ip_address);

    /**
     * Get interface MAC address
     * 
     * @param interface Interface name
     * @param mac_address Output MAC address (6 bytes)
     * @return True if successful
     */
    bool getInterfaceMac(const std::string& interface, uint8_t* mac_address);

    /**
     * Get the Ethernet destination MAC for ip_address.
     * Same-subnet: direct ARP table lookup (/proc/net/arp).
     * Off-subnet (e.g. cross-VPC via peering): resolves the local gateway from
     * /proc/net/route and returns the gateway MAC instead, so ENA delivers the
     * frame to the VPC router which forwards it over the peering connection.
     *
     * @param ip_address  Destination IP address string
     * @param mac_address Output MAC address (6 bytes)
     * @return True if a MAC was resolved (direct or via gateway)
     */
    bool getDestinationMac(const std::string& ip_address, uint8_t* mac_address);

    /**
     * Trigger ARP resolution for a destination IP
     * 
     * @param ip_address Destination IP address to resolve
     */
    void triggerArpResolution(const std::string& ip_address);

    /**
     * Setup CPU affinity for a thread
     * 
     * @param thread_id Thread to set affinity for
     * @param cpu_core CPU core to bind to
     * @return True if successful
     */
    bool setCpuAffinity(std::thread& thread, int cpu_core);
    
    /**
     * Initialize CPU core assignments for optimal performance
     */
    void initializeCpuCores();
    
    /**
     * Get cached destinations for a group (lock-free after first call per 100ms).
     * mcast mode: keyed by multicast group NBO.
     * Unicast mode: keyed by listen_ip_nbo_ (replicator's unicast address).
     * Returns a const ref to the thread-local vector — no copy on hot path.
     */
    const std::vector<Destination>& getCachedGroupDestinations(uint32_t group_nbo);

    /**
     * Rebuild thread-local destination cache.
     * mcast mode: from group_destinations_.
     * Unicast mode: from all_destinations_ keyed by listen_ip_nbo_.
     */
};

#endif // PACKET_REPLICATOR_HPP
