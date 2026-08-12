// mcast.c — XDP program for m2u-tagged multicast UDP interception
/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 *
 * POC use case: mock exchange instance sends real UDP multicast (224.x.x.x)
 * carried inside a plain unicast UDP packet to the feeder's private IP, tagged
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

// Light mcast->ucast tunnel tag ("M2CU"): 8-byte header {magic, group} that
// Kept in sync with mcast_send.cpp / Replicator.cpp / mcast_receive.cpp.
#define M2U_MAGIC 0x4D324355

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
    __u8 matched = 0;
    #pragma unroll
    for (int _idx = 0; _idx < MAX_GROUPS; _idx++) {
        if (!matched) {
            __u32 _k = (__u32)_idx;
            struct unicast_config *cfg = bpf_map_lookup_elem(&config_map, &_k);
            if (!cfg || cfg->target_ip == 0)
                continue;
            if (group == cfg->target_ip && udp->dest == cfg->target_port)
                matched = 1;
        }
    }

    if (!matched)
        return XDP_PASS;

    // ── Match — redirect whole frame to AF_XDP (zero-copy on ENA) ────────────
    // The userspace reader (replicator / mcast_receive) strips Eth/IP/UDP + the
    // 8-byte m2u header to reach the payload.
    __u32 queue_idx = ctx->rx_queue_index;
    return bpf_redirect_map(&xsks_map, queue_idx, XDP_DROP);
}
