/*
 * rtt - High-precision RTT measurement client for the AF_XDP benchmark.
 *
 * Measures round-trip latency through the packet replicator with minimal
 * measurement overhead. Timestamps are taken as close to the wire as possible:
 *
 *   Default: a single CLOCK_REALTIME domain on one host, so no clock sync is needed.
 *     TX: CLOCK_REALTIME (clock_gettime) sampled immediately before the send
 *         (kernel sendto, or the AF_XDP TX frame build with --xdp-tx).
 *     RX: kernel software timestamp (SOF_TIMESTAMPING_RX_SOFTWARE) = CLOCK_REALTIME
 *         (skb->tstamp = ktime_get_real), recorded by the stack in the NAPI receive
 *         path (netif_receive_skb / net_timestamp_check) right after the driver
 *         pulls the frame off the RX ring and builds the skb - before the socket
 *         receive-queue enqueue, so it excludes socket-queue + poll/schedule jitter.
 *         Falls back to a userspace CLOCK_REALTIME read if no cmsg timestamp.
 *     RTT = rx_realtime - tx_realtime.
 *
 *   --xdp-rx (optional): the RX time is stamped even earlier, at the XDP ingress
 *     hook, by the ucast XDP program via bpf_ktime_get_ns() = CLOCK_MONOTONIC,
 *     written into the echo payload; the client reads it and stamps TX with
 *     CLOCK_MONOTONIC to match. RTT = rx_mono - tx_mono. (On ENA this measured no
 *     lower than the kernel-SW path, since that stamp is already near-wire.)
 *
 *   No TSC and no PHC are used for the RTT. ENA has no TX hardware timestamp;
 *   PHC hardware RX timestamps live in a separate epoch and are used only for the
 *   one-way multicast path (mcast_receive), not here.
 *
 * Design:
 *   - Lock-free preallocated slot array indexed by sequence ID (no map, no mutex)
 *   - Busy-poll receive with SO_BUSY_POLL (no poll()/select() wakeup jitter)
 *   - CPU pinning for send and receive threads
 *   - In-process control subscription (no system() to external binary)
 *   - Warmup phase excluded from statistics
 *   - Coordinated omission tracking (intended vs actual send time)
 *   - Pacing via clock_nanosleep(TIMER_ABSTIME)
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <thread>
#include <chrono>

#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <net/if.h>
#include <immintrin.h>  // _mm_pause for busy-poll spin

#include "common/ControlPort.hpp"

// ── TX-only AF_XDP UDP sender for the RTT probe path (--xdp-tx) ────────────────
// Zero-copy transmit alternative to sendto(): builds a plain Eth|IPv4|UDP|payload
// frame once into every UMEM frame, then per packet writes only the sequence
// digits + stamps the TX timestamp immediately before xsk_ring_prod__submit
// (sfence-ordered before the NIC DMA). Removes the kernel TX stack (~3-5us) from
// the measured send leg. TX-only (XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD → no XDP
// program loaded), so it does NOT touch the RX datapath and coexists with a
// kernel receive socket. Bind to a queue the local replicator does not own
// (default != 0; RSS is pinned to queue 0, but TX egress is independent of RSS).
// Needs libxdp/libbpf → compiled only in full builds (guarded out of echo-mode).
#ifndef ECHO_MODE_ONLY

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>

#include <xdp/xsk.h>
#include <bpf/libbpf.h>

class XdpTxSend {
public:
    // Build the frame template and open a TX-only AF_XDP socket.
    // seq_off_in_payload: byte offset of the 10-digit ASCII sequence within the payload.
    bool init(const std::string& iface, int queue,
              const std::string& dst_ip, uint16_t dst_port,
              const char* payload, size_t payload_len, size_t seq_off_in_payload,
              std::string& err)
    {
        queue_ = queue;
        seq_frame_off_ = ETH_LEN + IP_LEN + UDP_LEN + seq_off_in_payload;

        iface_info src{};
        if (!get_iface_info(iface.c_str(), src)) { err = "get_iface_info failed for " + iface; return false; }

        uint8_t dst_mac[6];
        if (!resolve_mac(dst_ip.c_str(), iface.c_str(), dst_mac)) {
            err = "ARP resolution failed for " + dst_ip; return false;
        }

        uint32_t dst_ip_nbo = 0;
        inet_pton(AF_INET, dst_ip.c_str(), &dst_ip_nbo);

        pkt_len_ = build_udp_pkt(templ_, sizeof(templ_), src, dst_mac,
                                 dst_ip_nbo, htons(dst_port),
                                 payload, payload_len);
        if (pkt_len_ < 0) { err = "packet template too large"; return false; }

        { struct rlimit r = { RLIM_INFINITY, RLIM_INFINITY }; setrlimit(RLIMIT_MEMLOCK, &r); }

        if (posix_memalign(&umem_buf_, getpagesize(),
                           (size_t)NUM_FRAMES * FRAME_SIZE) != 0) {
            err = "posix_memalign failed"; return false;
        }
        for (uint32_t i = 0; i < NUM_FRAMES; i++)
            memcpy((uint8_t*)umem_buf_ + i * FRAME_SIZE, templ_, pkt_len_);

        const struct xsk_umem_config ucfg = {
            .fill_size = TX_RING, .comp_size = TX_RING,
            .frame_size = FRAME_SIZE, .frame_headroom = 0, .flags = 0,
        };
        int e = xsk_umem__create(&umem_, umem_buf_, (uint64_t)NUM_FRAMES * FRAME_SIZE,
                                 &fq_unused_, &cq_, &ucfg);
        if (e) { err = std::string("xsk_umem__create: ") + strerror(-e); return false; }

        struct xsk_socket_config xcfg = {
            .rx_size = 0, .tx_size = TX_RING,
            .libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
            .xdp_flags = XDP_FLAGS_DRV_MODE, .bind_flags = XDP_USE_NEED_WAKEUP,
        };
        e = xsk_socket__create(&xsk_, iface.c_str(), (uint32_t)queue_, umem_,
                               nullptr, &tx_, &xcfg);
        if (e) {  // native ZC failed — retry SKB (copy) mode
            xcfg.xdp_flags = XDP_FLAGS_SKB_MODE;
            e = xsk_socket__create(&xsk_, iface.c_str(), (uint32_t)queue_, umem_,
                                   nullptr, &tx_, &xcfg);
        }
        if (e) { err = std::string("xsk_socket__create (queue ") + std::to_string(queue_)
                       + "): " + strerror(-e); return false; }
        fd_ = xsk_socket__fd(xsk_);
        return true;
    }

    // Encode seq (10 ASCII digits), stamp TX time just before submit, transmit.
    // Returns the CLOCK_REALTIME ns captured at the stamp point.
    bool send(uint64_t seq, int64_t& send_realtime_ns)
    {
        drain_completions();

        uint32_t idx = 0;
        while (xsk_ring_prod__reserve(&tx_, 1, &idx) == 0) {
            if (xsk_ring_prod__needs_wakeup(&tx_))
                sendto(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
            drain_completions();
        }

        uint64_t addr  = ((uint64_t)seq % NUM_FRAMES) * FRAME_SIZE;
        uint8_t* frame = (uint8_t*)umem_buf_ + addr;

        // Write the 10-digit zero-padded sequence into the payload.
        uint8_t* p = frame + seq_frame_off_;
        uint64_t s = seq;
        for (int i = 9; i >= 0; --i) { p[i] = (uint8_t)('0' + (s % 10)); s /= 10; }

        // Stamp as close to the wire as possible.
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        send_realtime_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

        struct xdp_desc* d = xsk_ring_prod__tx_desc(&tx_, idx);
        d->addr = addr; d->len = (uint32_t)pkt_len_;
        asm volatile("sfence" ::: "memory");
        xsk_ring_prod__submit(&tx_, 1);
        outstanding_++;
        if (xsk_ring_prod__needs_wakeup(&tx_))
            sendto(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
        return true;
    }

    void close_()
    {
        if (xsk_)  { xsk_socket__delete(xsk_);  xsk_  = nullptr; }
        if (umem_) { xsk_umem__delete(umem_);   umem_ = nullptr; }
        if (umem_buf_) { free(umem_buf_); umem_buf_ = nullptr; }
    }
    ~XdpTxSend() { close_(); }

private:
    static constexpr int      ETH_LEN    = (int)sizeof(struct ethhdr);   // 14
    static constexpr int      IP_LEN     = (int)sizeof(struct iphdr);    // 20
    static constexpr int      UDP_LEN    = (int)sizeof(struct udphdr);   // 8
    static constexpr uint32_t NUM_FRAMES = 256;
    static constexpr uint32_t FRAME_SIZE = 4096;
    static constexpr uint32_t TX_RING    = 256;

    struct iface_info { uint8_t mac[6]; uint32_t ip; int ifindex; };

    void*             umem_buf_ = nullptr;
    struct xsk_umem*  umem_     = nullptr;
    struct xsk_socket* xsk_     = nullptr;
    struct xsk_ring_prod tx_{};
    struct xsk_ring_cons cq_{};
    struct xsk_ring_prod fq_unused_{};
    int      fd_          = -1;
    int      queue_       = 1;
    int      pkt_len_     = -1;
    int      seq_frame_off_ = 0;
    uint32_t outstanding_ = 0;
    uint8_t  templ_[2048];

    void drain_completions() {
        uint32_t idx = 0;
        uint32_t n = xsk_ring_cons__peek(&cq_, TX_RING, &idx);
        if (n) { xsk_ring_cons__release(&cq_, n); outstanding_ -= (n <= outstanding_ ? n : outstanding_); }
    }

    static uint16_t ip_csum(const void* data, int len) {
        const uint16_t* p = (const uint16_t*)data; uint32_t sum = 0;
        for (; len > 1; len -= 2) sum += *p++;
        if (len) sum += *(const uint8_t*)p;
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        return (uint16_t)~sum;
    }

    static bool get_iface_info(const char* iface, iface_info& out) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) return false;
        struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
        bool ok = ioctl(s, SIOCGIFHWADDR, &ifr) == 0;
        if (ok) memcpy(out.mac, ifr.ifr_hwaddr.sa_data, 6);
        if (ok && ioctl(s, SIOCGIFADDR, &ifr) == 0)
            out.ip = ntohl(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr);
        else ok = false;
        if (ok && ioctl(s, SIOCGIFINDEX, &ifr) == 0) out.ifindex = ifr.ifr_ifindex;
        else ok = false;
        ::close(s);
        return ok;
    }

    static bool resolve_mac(const char* dst_ip, const char* iface, uint8_t mac[6]) {
        // Trigger neighbour resolution, then read /proc/net/arp.
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface) + 1);
            struct sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(9);
            inet_pton(AF_INET, dst_ip, &a.sin_addr);
            connect(s, (struct sockaddr*)&a, sizeof(a));
            ::send(s, nullptr, 0, 0);
            ::close(s);
        }
        for (int attempt = 0; attempt < 20; ++attempt) {
            usleep(50000);
            FILE* f = fopen("/proc/net/arp", "r");
            if (!f) return false;
            char line[256]; fgets(line, sizeof(line), f);  // header
            bool found = false;
            while (fgets(line, sizeof(line), f)) {
                char ip[64], hw[64], t[64], fl[64], m[64], dev[64];
                if (sscanf(line, "%63s %63s %63s %63s %63s %63s", ip, t, fl, hw, m, dev) != 6) continue;
                if (strcmp(ip, dst_ip) != 0) continue;
                unsigned b[6];
                if (sscanf(hw, "%x:%x:%x:%x:%x:%x", &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) != 6) continue;
                if ((b[0]|b[1]|b[2]|b[3]|b[4]|b[5]) == 0) break;  // 00:00.. → not resolved yet
                for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
                found = true; break;
            }
            fclose(f);
            if (found) return true;
        }
        return false;
    }

    static int build_udp_pkt(uint8_t* buf, int max_buf, const iface_info& src,
                             const uint8_t dst_mac[6], uint32_t dst_ip_nbo,
                             uint16_t dst_port_nbo, const char* payload, size_t payload_len) {
        int total = ETH_LEN + IP_LEN + UDP_LEN + (int)payload_len;
        if (total > max_buf) return -1;
        memset(buf, 0, total);
        uint8_t* p = buf;

        auto* eth = (struct ethhdr*)p;
        memcpy(eth->h_dest, dst_mac, 6); memcpy(eth->h_source, src.mac, 6);
        eth->h_proto = htons(ETH_P_IP); p += ETH_LEN;

        auto* ip = (struct iphdr*)p;
        ip->ihl = 5; ip->version = 4; ip->tos = 0;
        ip->tot_len = htons((uint16_t)(IP_LEN + UDP_LEN + payload_len));
        ip->id = 0; ip->frag_off = htons(IP_DF); ip->ttl = 64;
        ip->protocol = IPPROTO_UDP; ip->saddr = htonl(src.ip); ip->daddr = dst_ip_nbo;
        ip->check = 0; ip->check = ip_csum(ip, IP_LEN); p += IP_LEN;

        auto* udp = (struct udphdr*)p;
        udp->source = dst_port_nbo;           // src port cosmetic; reuse dst for traceability
        udp->dest   = dst_port_nbo;
        udp->len    = htons((uint16_t)(UDP_LEN + payload_len));
        udp->check  = 0;                      // UDP checksum optional for IPv4
        p += UDP_LEN;

        memcpy(p, payload, payload_len);
        return total;
    }
};

#endif // ECHO_MODE_ONLY

// ---------------------------------------------------------------------------
// Timestamp source detection and RX timestamping
// ---------------------------------------------------------------------------
enum class RxTimestampMode { SW_KERNEL, USERSPACE };

static RxTimestampMode detect_timestamp_mode(int sock_fd) {
    // For RTT the send and receive timestamps must share one clock domain. The TX
    // side stamps CLOCK_REALTIME (clock_gettime before sendto), so we pick a RX
    // timestamp in the same domain:
    //   1. Kernel software RX timestamp (SOF_TIMESTAMPING_RX_SOFTWARE) - CLOCK_REALTIME
    //      (skb->tstamp = ktime_get_real), recorded in the NAPI receive path
    //      (netif_receive_skb / net_timestamp_check) as the driver hands the packet
    //      to the stack, before the socket receive-queue enqueue. Removes socket-queue
    //      + poll/schedule jitter while staying in the CLOCK_REALTIME domain.
    //   2. Userspace fallback (clock_gettime after recvmsg returns).
    //
    // HW PHC timestamps are deliberately NOT enabled here: they use a separate
    // wall-clock epoch and are only needed for one-way (multicast) latency.

    int sw_flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_TIMESTAMPING, &sw_flags, sizeof(sw_flags)) == 0) {
        // Also enable the older SO_TIMESTAMP as a belt-and-suspenders (some kernels
        // only populate SCM_TIMESTAMP, not SCM_TIMESTAMPING, for UDP)
        int one = 1;
        setsockopt(sock_fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof(one));
        std::cout << "Timestamp mode: kernel software RX (CLOCK_REALTIME, recorded in the NAPI netif_receive_skb path)"
                  << std::endl;
        return RxTimestampMode::SW_KERNEL;
    }

    std::cout << "Timestamp mode: userspace fallback (clock_gettime CLOCK_REALTIME after recvmsg)" << std::endl;
    return RxTimestampMode::USERSPACE;
}

// Extract RX timestamp from cmsg ancillary data (handles both new and old API)
static int64_t extract_rx_timestamp_ns(struct msghdr* msg) {
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        // New API: SO_TIMESTAMPING -> array of 3 timespecs [SW, deprecated, HW]
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
            struct timespec* ts = reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg));
            // Software RX timestamp (index 0), CLOCK_REALTIME. We do not enable
            // RX_HARDWARE, so the HW slot (index 2) is unused here.
            if (ts[0].tv_sec != 0 || ts[0].tv_nsec != 0) {
                return ts[0].tv_sec * 1000000000LL + ts[0].tv_nsec;
            }
        }
        // Old API: SO_TIMESTAMP -> struct timeval (microsecond precision, CLOCK_REALTIME-ish)
        // We convert to ns. Note: this is wall-clock, but for short RTTs the mono/wall
        // delta is negligible (no NTP step during a 10-second run).
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMP) {
            struct timeval* tv = reinterpret_cast<struct timeval*>(CMSG_DATA(cmsg));
            return tv->tv_sec * 1000000000LL + tv->tv_usec * 1000LL;
        }
    }
    return -1;  // No timestamp found
}

// ---------------------------------------------------------------------------
// Lock-free timing slots
// ---------------------------------------------------------------------------
struct alignas(64) TimingSlot {
    int64_t  send_realtime_ns;    // CLOCK_REALTIME at actual send (default-mode TX stamp)
    int64_t  send_mono_ns;        // CLOCK_MONOTONIC at actual send (--xdp-rx TX stamp)
    uint64_t intended_send_ns;    // intended send time, CLOCK_MONOTONIC (coordinated-omission baseline)
    int64_t  recv_ns;             // RX timestamp (ns): default = kernel-SW SO_TIMESTAMPING
                                  //   (CLOCK_REALTIME) or userspace CLOCK_REALTIME fallback;
                                  //   --xdp-rx = XDP bpf_ktime (CLOCK_MONOTONIC) from the payload
    uint8_t  received;            // 1 if response arrived
};

// ---------------------------------------------------------------------------
// Message encoding (wire-compatible with the existing market_data_provider_client)
// ---------------------------------------------------------------------------
static constexpr size_t TRADE_ID_OFFSET = 38;
static constexpr size_t TRADE_ID_DIGITS = 10;

static const char* MSG_TEMPLATE =
    R"({"e":"trade","E":1234567890123,"s":"BTC-USDT","t":0000000000,"p":"45000","q":"1.5","b":1000000001,"a":1000000002,"T":1234567890000,"S":"1","X":"MARKET"})";
static size_t MSG_LEN = 0;  // set at init

// rtt --xdp-rx wire header (KEEP IN SYNC with src/xdp/ucast.c): the first bytes of
// the UDP payload carry [0..3] magic, [4..11] xdp_rx_ns (stamped by the XDP program
// at echo ingress, host order). TRADE_ID_OFFSET (38) is past this header, so the
// seq digits are unaffected.
static constexpr uint32_t RTT_MAGIC = 0x58545452u;   // "RTTX" little-endian
static constexpr size_t   RTT_HDR_LEN = 12;
static char g_probe[256];                            // probe template (magic patched for --xdp-rx)

static void encode_message(char* buf, uint64_t seq_id) {
    memcpy(buf, g_probe, MSG_LEN);
    char* pos = buf + TRADE_ID_OFFSET;
    for (int i = TRADE_ID_DIGITS - 1; i >= 0; --i) {
        pos[i] = '0' + (seq_id % 10);
        seq_id /= 10;
    }
}

static uint64_t decode_seq_id(const char* buf, size_t len) {
    if (len < TRADE_ID_OFFSET + TRADE_ID_DIGITS) return 0;
    uint64_t id = 0;
    const char* pos = buf + TRADE_ID_OFFSET;
    for (size_t i = 0; i < TRADE_ID_DIGITS; ++i) {
        if (pos[i] < '0' || pos[i] > '9') return 0;
        id = id * 10 + (pos[i] - '0');
    }
    return id;
}

// ---------------------------------------------------------------------------
// Control protocol (in-process, no external binary)
// ---------------------------------------------------------------------------
static bool subscribe_to_replicator(const char* replicator_ip, [[maybe_unused]] uint16_t replicator_port,
                                    const char* local_ip, uint16_t local_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    // Set receive timeout for response
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server = {};
    server.sin_family = AF_INET;
    server.sin_port = htons(afxdp_control_port());  // control port
    inet_pton(AF_INET, replicator_ip, &server.sin_addr);

    // Wire format: [1=ADD][4B IP network order][2B port network order]
    uint8_t msg[7];
    msg[0] = 1;  // CTRL_ADD_DESTINATION
    inet_pton(AF_INET, local_ip, &msg[1]);
    uint16_t port_net = htons(local_port);
    memcpy(&msg[5], &port_net, 2);

    ssize_t sent = sendto(fd, msg, 7, 0, (struct sockaddr*)&server, sizeof(server));
    if (sent != 7) { close(fd); return false; }

    // Wait for ACK
    uint8_t ack = 0;
    ssize_t r = recv(fd, &ack, 1, 0);
    close(fd);

    if (r == 1 && ack == 1) {
        std::cout << "Subscribed to replicator at " << replicator_ip << ":9000" << std::endl;
        return true;
    }

    // Retry once
    std::cerr << "Subscription failed (ack=" << (int)ack << "), retrying..." << std::endl;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sent = sendto(fd, msg, 7, 0, (struct sockaddr*)&server, sizeof(server));
    if (sent == 7) { r = recv(fd, &ack, 1, 0); }
    close(fd);
    return (r == 1 && ack == 1);
}

// ---------------------------------------------------------------------------
// CPU pinning
// ---------------------------------------------------------------------------
static void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) == 0) {
        std::cout << "  pinned to CPU " << cpu << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Real-time scheduling + memory locking
// ---------------------------------------------------------------------------
static void enable_realtime() {
    // Set SCHED_FIFO priority 80 (not 99 -- leave room for kernel threads)
    struct sched_param param = {};
    param.sched_priority = 80;
    if (sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
        std::cout << "  SCHED_FIFO priority 80 enabled" << std::endl;
    } else {
        std::cerr << "  Warning: SCHED_FIFO failed (need root/CAP_SYS_NICE), continuing with SCHED_OTHER" << std::endl;
    }

    // Lock all current and future pages to prevent page faults during measurement
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        std::cout << "  mlockall enabled (no page faults during measurement)" << std::endl;
    } else {
        std::cerr << "  Warning: mlockall failed (need root/CAP_IPC_LOCK)" << std::endl;
    }
}

// Safety alarm: auto-kill if stuck in RT spin loop (prevents system lockup)
static void alarm_handler(int) {
    // Force exit -- RT thread may be spinning and blocking normal shutdown
    _exit(2);
}

static void set_safety_alarm(uint64_t total_msgs, uint64_t rate_per_sec) {
    // Expected runtime + generous 30s headroom
    unsigned int timeout_sec = static_cast<unsigned int>(total_msgs / rate_per_sec) + 30;
    signal(SIGALRM, alarm_handler);
    alarm(timeout_sec);
    std::cout << "  safety alarm: " << timeout_sec << "s" << std::endl;
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static volatile bool g_running = true;
static void sig_handler(int) { g_running = false; }

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Separate optional flags (--xdp-tx[=queue], --iface <name>) from positionals.
    bool use_xdp_tx = false;
    bool xdp_rx = false;   // --xdp-rx: read the XDP-stamped ingress time from the payload
    [[maybe_unused]] int xdp_queue = 1;   // avoid queue 0 (owned by the local ucast replicator's AF_XDP socket)
    std::string iface;
    std::vector<const char*> pos;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--xdp-tx")                    { use_xdp_tx = true; }
        else if (a.rfind("--xdp-tx=", 0) == 0)  { use_xdp_tx = true; xdp_queue = atoi(a.c_str() + 9); }
        else if (a == "--iface" && i + 1 < argc){ iface = argv[++i]; }
        else if (a == "--xdp-rx")               { xdp_rx = true; }
        else                                     { pos.push_back(argv[i]); }
    }

    if (pos.size() < 7 || pos.size() > 10) {
        std::cerr << "Usage: " << pos[0]
                  << " <replicator_ip> <replicator_port> <local_ip> <local_port>"
                  << " <total_messages> <rate_per_sec>"
                  << " [warmup=10000] [send_cpu=1] [recv_cpu=2]"
                  << " [--xdp-tx[=queue]] [--iface <name>] [--xdp-rx]" << std::endl;
        return 1;
    }

    const char* replicator_ip = pos[1];
    uint16_t    replicator_port = static_cast<uint16_t>(atoi(pos[2]));
    const char* local_ip = pos[3];
    uint16_t    local_port = static_cast<uint16_t>(atoi(pos[4]));
    uint64_t    total_msgs = strtoull(pos[5], nullptr, 10);
    uint64_t    rate_per_sec = strtoull(pos[6], nullptr, 10);
    uint64_t    warmup = (pos.size() > 7) ? strtoull(pos[7], nullptr, 10) : 10000;
    int         send_cpu = (pos.size() > 8) ? atoi(pos[8]) : 1;
    int         recv_cpu = (pos.size() > 9) ? atoi(pos[9]) : 2;

#ifdef ECHO_MODE_ONLY
    if (use_xdp_tx) { std::cerr << "--xdp-tx not available in this (echo-mode) build" << std::endl; return 1; }
#endif
    if (use_xdp_tx && iface.empty()) { std::cerr << "--xdp-tx requires --iface <name>" << std::endl; return 1; }

    if (total_msgs == 0 || rate_per_sec == 0) {
        std::cerr << "total_messages and rate must be positive" << std::endl;
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // Initialize message template length + probe buffer. For --xdp-rx, patch the
    // magic into payload[0..3] and zero the stamp slot [4..11]; the XDP program on
    // the echo's ingress writes the RX time there. TRADE_ID_OFFSET (38) is past it.
    MSG_LEN = strlen(MSG_TEMPLATE);
    memcpy(g_probe, MSG_TEMPLATE, MSG_LEN);
    if (xdp_rx) {
        uint32_t m = RTT_MAGIC; memcpy(g_probe, &m, sizeof(m));
        memset(g_probe + 4, 0, 8);
    }

    // Allocate timing slots (warmup + measured messages)
    uint64_t slot_count = warmup + total_msgs;
    std::vector<TimingSlot> slots(slot_count);
    memset(slots.data(), 0, slot_count * sizeof(TimingSlot));

    // Create and bind receive socket
    int recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd < 0) { perror("socket"); return 1; }

    // Enable busy poll (kernel spins polling NIC queue instead of sleeping for IRQ)
    // With SCHED_FIFO, we can afford aggressive spinning -- eliminates IRQ wakeup latency
    int busy_poll_us = 100;
    setsockopt(recv_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us));
    int prefer_busy_poll = 1;
    setsockopt(recv_fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer_busy_poll, sizeof(prefer_busy_poll));
    int busy_budget = 256;  // packets to process per busy-poll cycle
    setsockopt(recv_fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &busy_budget, sizeof(busy_budget));

    // Increase receive buffer
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(recv_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(local_port);
    inet_pton(AF_INET, local_ip, &bind_addr.sin_addr);
    if (bind(recv_fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind"); close(recv_fd); return 1;
    }

    // Detect and configure RX timestamp mode. --xdp-rx reads the XDP-stamped time
    // from the payload instead, so skip kernel/HW RX timestamping.
    RxTimestampMode ts_mode = RxTimestampMode::USERSPACE;
    if (xdp_rx) {
        std::cout << "Timestamp mode: XDP RX ktime (bpf_ktime_get_ns MONOTONIC, stamped at XDP ingress)" << std::endl;
    } else {
        ts_mode = detect_timestamp_mode(recv_fd);
    }

    // Subscribe to the replicator
    if (!subscribe_to_replicator(replicator_ip, replicator_port, local_ip, local_port)) {
        std::cerr << "Failed to subscribe to replicator" << std::endl;
        close(recv_fd); return 1;
    }

    // Create send socket
    int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_fd < 0) { perror("send socket"); close(recv_fd); return 1; }

    struct sockaddr_in dest_addr = {};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(replicator_port);
    inet_pton(AF_INET, replicator_ip, &dest_addr.sin_addr);

    // --- Receiver thread ---
    std::atomic<uint64_t> total_received{0};
    std::atomic<bool> recv_started{false};

    // Set safety alarm BEFORE enabling RT (alarm fires even if RT thread is spinning)
    set_safety_alarm(total_msgs + warmup, rate_per_sec);

    std::thread receiver([&]() {
        pin_to_cpu(recv_cpu);
        enable_realtime();  // RT scheduling on receiver thread (latency-critical)
        recv_started.store(true);

        char buf[2048];
        char ctrl[256];
        struct iovec iov = { buf, sizeof(buf) };
        struct msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);

        while (g_running) {
            msg.msg_controllen = sizeof(ctrl);  // reset each iteration
            ssize_t n = recvmsg(recv_fd, &msg, MSG_DONTWAIT);
            if (n <= 0) {
                _mm_pause();
                continue;
            }

            // Get RX timestamp
            int64_t rx_ns;
            if (xdp_rx) {
                // XDP stamped bpf_ktime_get_ns() into payload[4..11] (host order);
                // 0 means it was not stamped (old ucast.o / off path) -> dropped by sanity.
                uint64_t xrx = 0;
                if (n >= (ssize_t)RTT_HDR_LEN) memcpy(&xrx, buf + 4, sizeof(xrx));
                rx_ns = static_cast<int64_t>(xrx);
            } else if (ts_mode == RxTimestampMode::USERSPACE) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                rx_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
            } else {
                rx_ns = extract_rx_timestamp_ns(&msg);
                if (rx_ns < 0) {
                    // Fallback to userspace (same CLOCK_REALTIME domain) if cmsg missing
                    struct timespec now;
                    clock_gettime(CLOCK_REALTIME, &now);
                    rx_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
                }
            }

            uint64_t seq = decode_seq_id(buf, n);
            if (seq > 0 && seq <= slot_count) {
                slots[seq - 1].recv_ns = rx_ns;
                slots[seq - 1].received = 1;
                total_received.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Wait for receiver to start
    while (!recv_started.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // --- Sender ---
    std::cout << "Running: " << total_msgs << " messages + " << warmup << " warmup @ "
              << rate_per_sec << " msg/sec" << std::endl;
    std::cout << "  send_cpu=" << send_cpu << " recv_cpu=" << recv_cpu << std::endl;

    pin_to_cpu(send_cpu);
    enable_realtime();  // RT scheduling on sender thread

#ifndef ECHO_MODE_ONLY
    XdpTxSend xtx;
    if (use_xdp_tx) {
        std::string xerr;
        if (!xtx.init(iface, xdp_queue, replicator_ip, replicator_port,
                      g_probe, MSG_LEN, TRADE_ID_OFFSET, xerr)) {
            std::cerr << "AF_XDP TX init failed (" << xerr
                      << "); falling back to kernel sendto" << std::endl;
            use_xdp_tx = false;
        } else {
            std::cout << "AF_XDP TX enabled on " << iface << " queue " << xdp_queue << std::endl;
        }
    }
#endif

    char msg_buf[512];
    uint64_t interval_ns = 1000000000ULL / rate_per_sec;

    struct timespec start_ts, start_rt;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    clock_gettime(CLOCK_REALTIME, &start_rt);
    uint64_t start_ns = start_ts.tv_sec * 1000000000ULL + start_ts.tv_nsec;
    // Constant MONOTONIC->REALTIME offset, to express the paced (MONOTONIC) intended
    // send time in the default RX domain (CLOCK_REALTIME) for the response RTT.
    const int64_t mono_to_real_off = (start_rt.tv_sec * 1000000000LL + start_rt.tv_nsec)
                                   - static_cast<int64_t>(start_ns);

    for (uint64_t i = 1; i <= slot_count && g_running; ++i) {
        uint64_t intended_ns = start_ns + (i - 1) * interval_ns;

        // Pace via absolute deadline
        struct timespec deadline;
        deadline.tv_sec = intended_ns / 1000000000ULL;
        deadline.tv_nsec = intended_ns % 1000000000ULL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);

        // Actual-send CLOCK_MONOTONIC (used by --xdp-rx, whose RX stamp is bpf_ktime MONOTONIC)
        int64_t send_mono_ns = 0;
        if (xdp_rx) { struct timespec m; clock_gettime(CLOCK_MONOTONIC, &m); send_mono_ns = m.tv_sec * 1000000000LL + m.tv_nsec; }

        // Send: AF_XDP zero-copy TX (kernel-bypass) or kernel sendto.
        int64_t send_rt_ns = 0;
#ifndef ECHO_MODE_ONLY
        if (use_xdp_tx) {
            xtx.send(i, send_rt_ns);
        } else
#endif
        {
            encode_message(msg_buf, i);
            struct timespec send_ts;
            clock_gettime(CLOCK_REALTIME, &send_ts);
            send_rt_ns = send_ts.tv_sec * 1000000000LL + send_ts.tv_nsec;
            sendto(send_fd, msg_buf, MSG_LEN, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        }

        // TX stamp: CLOCK_REALTIME (matches the kernel-SW RX domain). The
        // CLOCK_MONOTONIC stamp above is used only by --xdp-rx.
        slots[i - 1].send_realtime_ns = send_rt_ns;
        slots[i - 1].send_mono_ns = send_mono_ns;
        slots[i - 1].intended_send_ns = intended_ns;

        // Progress every 10K
        if (i % 10000 == 0) {
            std::cout << "  sent " << i << "/" << slot_count
                      << " (received=" << total_received.load() << ")" << std::endl;
        }
    }

    // Wait for stragglers (up to 3 seconds)
    std::cout << "Sending complete. Waiting for responses..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    g_running = false;
    receiver.join();
    close(send_fd);
    close(recv_fd);

    // --- Compute statistics (skip warmup) ---
    std::vector<int64_t> service_rtts;   // recv_ns - send_ref (same clock domain per mode)
    std::vector<int64_t> response_rtts;  // recv_ns - intended_send_ns (coordinated omission)
    uint64_t lost = 0;

    for (uint64_t i = warmup; i < slot_count; ++i) {
        if (!slots[i].received) { ++lost; continue; }

        // Same clock domain per mode: default = CLOCK_REALTIME, --xdp-rx = CLOCK_MONOTONIC.
        int64_t send_ref     = xdp_rx ? slots[i].send_mono_ns : slots[i].send_realtime_ns;
        int64_t intended_ref = xdp_rx ? static_cast<int64_t>(slots[i].intended_send_ns)
                                      : static_cast<int64_t>(slots[i].intended_send_ns) + mono_to_real_off;
        int64_t rtt = slots[i].recv_ns - send_ref;
        if (rtt > 0 && rtt < 100000000) {  // sanity: 0 < RTT < 100ms (unstamped -> rtt<=0, dropped)
            service_rtts.push_back(rtt);
        }

        int64_t resp_rtt = slots[i].recv_ns - intended_ref;
        if (resp_rtt > 0 && resp_rtt < 100000000) {
            response_rtts.push_back(resp_rtt);
        }
    }

    // Sort for percentiles
    std::sort(service_rtts.begin(), service_rtts.end());
    std::sort(response_rtts.begin(), response_rtts.end());

    auto percentile = [](const std::vector<int64_t>& v, double p) -> int64_t {
        if (v.empty()) return 0;
        size_t idx = std::min(v.size() - 1, static_cast<size_t>(v.size() * p / 100.0));
        return v[idx];
    };

    auto mean = [](const std::vector<int64_t>& v) -> double {
        if (v.empty()) return 0;
        return static_cast<double>(std::accumulate(v.begin(), v.end(), 0LL)) / v.size();
    };

    // --- Print results ---
    uint64_t measured = total_msgs;
    std::cout << "\n=== RTT Latency Results  ===" << std::endl;
    std::cout << "Messages: " << measured << " measured (+ " << warmup << " warmup)" << std::endl;
    std::cout << "Rate: " << rate_per_sec << " msg/sec" << std::endl;
    std::cout << "Lost: " << lost << " (" << (100.0 * lost / measured) << "%)" << std::endl;
    if (xdp_rx) {
        std::cout << "RX timestamp: XDP bpf_ktime_get_ns (CLOCK_MONOTONIC, XDP ingress hook)" << std::endl;
        std::cout << "TX timestamp: CLOCK_MONOTONIC (matches --xdp-rx RX domain)" << std::endl;
    } else {
        std::cout << "RX timestamp: "
                  << (ts_mode == RxTimestampMode::SW_KERNEL
                        ? "kernel software SO_TIMESTAMPING (CLOCK_REALTIME, NAPI netif_receive_skb)"
                        : "userspace clock_gettime (CLOCK_REALTIME)")
                  << std::endl;
        std::cout << "TX timestamp: CLOCK_REALTIME (clock_gettime before send)" << std::endl;
    }

    if (!service_rtts.empty()) {
        std::cout << "\nService-time RTT (recv - actual_send):" << std::endl;
        std::cout << "  Min:    " << service_rtts.front() / 1000 << " us" << std::endl;
        std::cout << "  Mean:   " << static_cast<int64_t>(mean(service_rtts) / 1000) << " us" << std::endl;
        std::cout << "  p50:    " << percentile(service_rtts, 50) / 1000 << " us" << std::endl;
        std::cout << "  p90:    " << percentile(service_rtts, 90) / 1000 << " us" << std::endl;
        std::cout << "  p95:    " << percentile(service_rtts, 95) / 1000 << " us" << std::endl;
        std::cout << "  p99:    " << percentile(service_rtts, 99) / 1000 << " us" << std::endl;
        std::cout << "  p99.9:  " << percentile(service_rtts, 99.9) / 1000 << " us" << std::endl;
        std::cout << "  Max:    " << service_rtts.back() / 1000 << " us" << std::endl;
    }

    if (!response_rtts.empty()) {
        std::cout << "\nResponse-time RTT (recv - intended_send, incl. coordinated omission):" << std::endl;
        std::cout << "  p50:    " << percentile(response_rtts, 50) / 1000 << " us" << std::endl;
        std::cout << "  p99:    " << percentile(response_rtts, 99) / 1000 << " us" << std::endl;
        std::cout << "  p99.9:  " << percentile(response_rtts, 99.9) / 1000 << " us" << std::endl;
    }

    std::cout << "====================================================" << std::endl;

    // --- Write JSON summary ---
    std::string json_file = "/tmp/rtt_results.json";
    FILE* jf = fopen(json_file.c_str(), "w");
    if (jf) {
        fprintf(jf, "{\n");
        fprintf(jf, "  \"client\": \"rtt\",\n");
        fprintf(jf, "  \"messages\": %lu,\n", measured);
        fprintf(jf, "  \"warmup\": %lu,\n", warmup);
        fprintf(jf, "  \"rate_mps\": %lu,\n", rate_per_sec);
        fprintf(jf, "  \"lost\": %lu,\n", lost);
        fprintf(jf, "  \"loss_pct\": %.4f,\n", 100.0 * lost / measured);
        fprintf(jf, "  \"timestamp_rx\": \"%s\",\n",
                xdp_rx ? "xdp_ktime_mono" :
                ts_mode == RxTimestampMode::SW_KERNEL ? "kernel_sw" : "userspace");
        fprintf(jf, "  \"timestamp_tx\": \"%s\",\n", xdp_rx ? "clock_monotonic" : "clock_realtime");
        fprintf(jf, "  \"tx_path\": \"%s\"%s\n", use_xdp_tx ? "af_xdp" : "kernel",
                (service_rtts.empty() && response_rtts.empty()) ? "" : ",");
        if (!service_rtts.empty()) {
            fprintf(jf, "  \"service_rtt_us\": {\n");
            fprintf(jf, "    \"min\": %ld,\n", service_rtts.front() / 1000);
            fprintf(jf, "    \"mean\": %ld,\n", static_cast<int64_t>(mean(service_rtts) / 1000));
            fprintf(jf, "    \"p50\": %ld,\n", percentile(service_rtts, 50) / 1000);
            fprintf(jf, "    \"p90\": %ld,\n", percentile(service_rtts, 90) / 1000);
            fprintf(jf, "    \"p95\": %ld,\n", percentile(service_rtts, 95) / 1000);
            fprintf(jf, "    \"p99\": %ld,\n", percentile(service_rtts, 99) / 1000);
            fprintf(jf, "    \"p999\": %ld,\n", percentile(service_rtts, 99.9) / 1000);
            fprintf(jf, "    \"max\": %ld\n", service_rtts.back() / 1000);
            fprintf(jf, "  }%s\n", response_rtts.empty() ? "" : ",");
        }
        if (!response_rtts.empty()) {
            fprintf(jf, "  \"response_rtt_us\": {\n");
            fprintf(jf, "    \"p50\": %ld,\n", percentile(response_rtts, 50) / 1000);
            fprintf(jf, "    \"p99\": %ld,\n", percentile(response_rtts, 99) / 1000);
            fprintf(jf, "    \"p999\": %ld\n", percentile(response_rtts, 99.9) / 1000);
            fprintf(jf, "  }\n");
        }
        fprintf(jf, "}\n");
        fclose(jf);
        std::cout << "\nJSON results written to " << json_file << std::endl;
    }

    return (lost > measured / 10) ? 1 : 0;  // exit 1 if >10% loss
}
