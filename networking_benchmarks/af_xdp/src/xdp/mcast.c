// mcast.c — XDP program for m2u-tagged multicast UDP interception
/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 *
 * A sender instance sends real UDP multicast (224.x.x.x)
 * carried inside a plain unicast UDP packet to a replicator's private IP, tagged
 * with a light 8-byte "m2u" tunnel header { magic, group }.  This program
 * intercepts that frame on eth0 and redirects it to AF_XDP, preserving
 * XDP_ZEROCOPY on the ENA physical NIC.
 *
 * Packet layout matched (flat 8-byte m2u tunnel header):
 *   Ethernet / IPv4 (proto=17) / UDP / m2u{ magic(4), group(4) } / payload
 *
 * The flat header keeps the fast path to Eth/IP/UDP + an 8-byte tag (no outer
 * IP parse — the fast path is Eth/IP/UDP + an 8-byte tag check.
 * Same config_map / xsks_map layout as ucast_filter.c —
 * target_ip is the multicast group (read from the m2u header), target_port is
 * the UDP destination port.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
#endif

// Light mcast->ucast tunnel tag ("M2CU"): 8-byte header {magic, group}.
// The wire format lives in ONE place — src/common/wire.h — shared with
// mcast_send.cpp / DataPath.cpp / mcast_receive.cpp so the offsets cannot drift.
#include "../common/wire.h"
#define M2U_MAGIC WIRE_M2U_MAGIC

// Required for logging in XDP programs
#define DEBUG 0
#define bpf_debug(fmt, ...)                 \
    ({                                      \
        if (DEBUG)                          \
            bpf_printk(fmt, ##__VA_ARGS__); \
    })

char _license[] SEC("license") = "GPL";

// Statistics map — same layout as the other filter programs
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} stats SEC(".maps");

// XSK map: queue index -> AF_XDP socket fd
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 256);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

// Maximum number of multicast groups a single replicator instance can intercept.
// config_map slots may be sparse (dynamic add/remove); target_ip == 0 marks an unused
// slot — the scan always checks all MAX_GROUPS entries via continue, not break.
#define MAX_GROUPS 16

// Configuration: inner multicast group + UDP port to intercept
struct unicast_config {
    __u32 target_ip;    // inner multicast group address (network byte order; 0 = unused)
    __u16 target_port;  // inner UDP destination port (network byte order)
    __u16 padding;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, MAX_GROUPS);
    __type(key, __u32);
    __type(value, struct unicast_config);
} config_map SEC(".maps");

// Kernel-side forward target (REPLICATOR_FWD_MODE=kernel). When enabled != 0 for
// a matched config slot, the XDP program rewrites the frame's L2/L3/L4 headers for
// this destination, stamps replicator_ns, and XDP_TX's it back out the NIC —
// forwarding the packet entirely in the kernel, no AF_XDP/userspace round-trip.
// Populated from userspace (Replicator) on join when in kernel mode. Parallel to
// config_map (same slot index). All addresses network byte order.
struct fwd_target {
    __u8  dmac[6];   // destination MAC
    __u8  smac[6];   // replicator (source) MAC
    __u32 dip;       // destination IP
    __u32 sip;       // replicator (source) IP
    __u16 dport;     // destination UDP port
    __u16 sport;     // replicator (source) UDP port
    __u8  enabled;   // 0 = redirect to XSK (default); 1 = kernel XDP_TX forward
    __u8  pad[3];
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, MAX_GROUPS);
    __type(key, __u32);
    __type(value, struct fwd_target);
} fwd_map SEC(".maps");

// Update statistics counter
static inline void increment_counter(int index)
{
    __u32 key = index;
    __u64 *value, init_val = 1;

    value = bpf_map_lookup_elem(&stats, &key);
    if (value)
        (*value)++;
    else
        bpf_map_update_elem(&stats, &key, &init_val, BPF_ANY);
}

SEC("xdp")
int mcast(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data     = (void *)(long)ctx->data;

    // ── Ethernet ──────────────────────────────────────────────────────────────
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    // ── IPv4 ──────────────────────────────────────────────────────────────────
    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_PASS;

    // Fast-path: only UDP; everything else untouched
    if (iph->protocol != IPPROTO_UDP)
        return XDP_PASS;

    __u32 ip_len = iph->ihl * 4;
    if (ip_len < 20 || ip_len > 60)
        return XDP_PASS;

    // The XDP_TX path recomputes the checksum over exactly WIRE_IP_IHL_WORDS bytes.
    // Pass frames with IP options to the kernel; they are uncommon in this workload.
    if (iph->ihl != WIRE_IP_IHL_NO_OPTIONS)
        return XDP_PASS;

    // ── UDP ────────────────────────────────────────────────────────────────
    struct udphdr *udp = (void *)iph + ip_len;
    if ((void *)(udp + 1) > data_end)
        return XDP_PASS;

    // ── m2u tunnel header: magic(4) + multicast group(4), network byte order ──
    __u8 *m2u = (__u8 *)(udp + 1);
    if (m2u + 8 > ((__u8 *)data_end))
        return XDP_PASS;

    __u32 magic = ((__u32)m2u[0] << 24) | ((__u32)m2u[1] << 16) |
                  ((__u32)m2u[2] << 8)  |  (__u32)m2u[3];
    if (magic != M2U_MAGIC)
        return XDP_PASS;

    __u32 group;
    __builtin_memcpy(&group, m2u + 4, 4);   // network byte order group

    // ── Config map scan: match {group, udp dst port} up to MAX_GROUPS ─────────
    // Entries are populated sequentially; target_ip == 0 means unused slot.
    int matched_idx = -1;
    #pragma unroll
    for (int _idx = 0; _idx < MAX_GROUPS; _idx++) {
        if (matched_idx < 0) {
            __u32 _k = (__u32)_idx;
            struct unicast_config *cfg = bpf_map_lookup_elem(&config_map, &_k);
            if (!cfg || cfg->target_ip == 0)
                continue;
            if (group == cfg->target_ip && udp->dest == cfg->target_port)
                matched_idx = _idx;
        }
    }

    if (matched_idx < 0)
        return XDP_PASS;

    // ── Kernel-side forward (REPLICATOR_FWD_MODE=kernel) ──────────────────────
    // If a forward target is enabled for the matched slot, rewrite the frame's
    // headers for the destination and XDP_TX it back out the NIC — no AF_XDP or
    // userspace round-trip.
    //
    // Kernel forward (REPLICATOR_FWD_MODE=kernel): rewrite headers and XDP_TX.
    // Userspace only enables this when exactly one destination is registered;
    // fan-out to multiple destinations requires bpf_clone_redirect(), which is
    // not used here. copy or inplace mode handles multi-destination workloads.
    {
        __u32 fk = (__u32)matched_idx;
        struct fwd_target *ft = bpf_map_lookup_elem(&fwd_map, &fk);
        if (ft && ft->enabled) {
            // L2: dst = destination, src = replicator
            __builtin_memcpy(eth->h_dest,   ft->dmac, 6);
            __builtin_memcpy(eth->h_source, ft->smac, 6);
            // L3: rewrite IPs + recompute the 20-byte IPv4 header checksum
            iph->daddr = ft->dip;
            iph->saddr = ft->sip;
            iph->check = 0;
            __u32 csum = 0;
            __u16 *ipw = (__u16 *)iph;
            #pragma unroll
            for (int i = 0; i < 10; i++)
                csum += ipw[i];
            csum = (csum & 0xffff) + (csum >> 16);
            csum = (csum & 0xffff) + (csum >> 16);
            iph->check = (__u16)~csum;
            // L4: rewrite ports, disable UDP checksum (optional for IPv4)
            udp->dest   = ft->dport;
            udp->source = ft->sport;
            udp->check  = 0;
            // NOTE: replicator_ns is NOT stamped here — BPF has no CLOCK_REALTIME
            // helper (only MONOTONIC bpf_ktime_get_ns, a different epoch than the
            // source/receiver's CLOCK_REALTIME). Left as the source's zero, so the
            // receiver reports the valid one-way total and simply omits the hop
            // split for kernel-forwarded packets (kernel proc time is ~0 anyway).
            increment_counter(2);
            return XDP_TX;
        }
    }

    // ── Default: redirect whole frame to AF_XDP (zero-copy on ENA) ───────────
    // The userspace reader (replicator / mcast_receive) strips Eth/IP/UDP + the
    // 8-byte m2u header to reach the payload.
    //
    // No XSK registered for this queue yet (e.g. the brief window between
    // XDP program load and XSK map update): fall through to the kernel stack.
    __u32 queue_idx = ctx->rx_queue_index;
    return bpf_redirect_map(&xsks_map, queue_idx, XDP_PASS);
}
