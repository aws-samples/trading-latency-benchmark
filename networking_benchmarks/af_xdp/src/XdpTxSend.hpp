/*
 * XdpTxSend.hpp — TX-only AF_XDP UDP sender for the RTT probe path.
 *
 * Zero-copy transmit alternative to sendto() for rtt: builds a plain
 * Eth | IPv4 | UDP | payload frame once into every UMEM frame, then per packet
 * writes only the sequence digits + stamps the TX timestamp immediately before
 * xsk_ring_prod__submit (sfence-ordered before the NIC DMA). Removes the kernel
 * TX stack (~3-5us) from the measured send leg.
 *
 * TX-only, XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD → no XDP program is loaded, so it
 * does NOT touch the RX datapath and coexists with a kernel receive socket. Bind
 * to a queue the local replicator does not own (default != 0; RSS is pinned to
 * queue 0 for the ucast replicator, but TX egress is independent of RSS).
 *
 * Compiled only in full builds (needs libxdp/libbpf); excluded from kernel-mode.
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 */
#pragma once
#ifndef KERNEL_MODE_ONLY

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
    // Returns the TSC and CLOCK_REALTIME ns captured at the stamp point.
    bool send(uint64_t seq, uint64_t& send_tsc, int64_t& send_realtime_ns)
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
        send_tsc = rdtsc_();
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

    static inline uint64_t rdtsc_() {
        uint32_t lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

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

#endif // KERNEL_MODE_ONLY
