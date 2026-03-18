// gre_filter.c — XDP program for GRE-encapsulated multicast UDP interception
/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 *
 * POC use case: mock exchange instance sends real UDP multicast (224.x.x.x)
 * encapsulated in a minimal GRE (IP proto 47) unicast tunnel to the feeder's
 * private IP.  This program intercepts the outer GRE frame on eth0 before the
 * kernel's ip_gre module decapsulates it, preserving XDP_ZEROCOPY on the ENA
 * physical NIC.
 *
 * Packet layout matched:
 *   Ethernet / outer IPv4 (proto=47) / GRE / inner IPv4 (proto=17) / UDP / payload
 *
 * Optional GRE fields (checksum, key, sequence) are handled via the flags word.
 * Same config_map / xsks_map layout as unicast_filter.c and multicast_filter.c —
 * no userspace config changes needed: target_ip is the inner multicast group,
 * target_port is the inner UDP destination port.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#ifndef IPPROTO_GRE
#define IPPROTO_GRE 47
#endif

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

// Configuration: inner multicast group + UDP port to intercept
struct unicast_config {
    __u32 target_ip;    // inner multicast group address (network byte order)
    __u16 target_port;  // inner UDP destination port (network byte order)
    __u16 padding;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct unicast_config);
} config_map SEC(".maps");

SEC("xdp")
int gre_filter(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data     = (void *)(long)ctx->data;

    // ── Ethernet ──────────────────────────────────────────────────────────────
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    // ── Outer IPv4 ────────────────────────────────────────────────────────────
    struct iphdr *outer_iph = (void *)(eth + 1);
    if ((void *)(outer_iph + 1) > data_end)
        return XDP_PASS;

    // Fast-path: only process GRE (proto 47); everything else untouched
    if (outer_iph->protocol != IPPROTO_GRE)
        return XDP_PASS;

    __u32 outer_ip_len = outer_iph->ihl * 4;
    if (outer_ip_len < 20 || outer_ip_len > 60)
        return XDP_PASS;

    // ── GRE fixed header (4 bytes: flags word + protocol word) ───────────────
    __u8 *gre = (void *)outer_iph + outer_ip_len;
    if (gre + 4 > ((__u8 *)data_end))
        return XDP_PASS;

    __u16 gre_flags = (__u16)(gre[0] << 8) | gre[1];
    __u16 gre_proto = (__u16)(gre[2] << 8) | gre[3];

    // GRE must carry IPv4
    if (gre_proto != ETH_P_IP)
        return XDP_PASS;

    // Variable GRE header size based on optional fields.
    // Bit 15 (C): checksum+reserved (+4), bit 13 (K): key (+4), bit 12 (S): seq (+4)
    __u32 gre_len = 4;
    if (gre_flags & 0x8000) gre_len += 4;
    if (gre_flags & 0x2000) gre_len += 4;
    if (gre_flags & 0x1000) gre_len += 4;
    // Maximum possible GRE header is 16 bytes; cap for the verifier
    if (gre_len > 16)
        return XDP_PASS;

    // ── Inner IPv4 ────────────────────────────────────────────────────────────
    struct iphdr *inner_iph = (void *)gre + gre_len;
    if ((void *)(inner_iph + 1) > data_end)
        return XDP_PASS;

    if (inner_iph->protocol != IPPROTO_UDP)
        return XDP_PASS;

    // Inner destination must be in the multicast range 224.0.0.0/4
    if ((bpf_ntohl(inner_iph->daddr) & 0xF0000000) != 0xE0000000)
        return XDP_PASS;

    // ── Config map lookup: specific group + port ──────────────────────────────
    __u32 key = 0;
    struct unicast_config *config = bpf_map_lookup_elem(&config_map, &key);
    if (!config)
        return XDP_PASS;

    if (inner_iph->daddr != config->target_ip)
        return XDP_PASS;

    // ── Inner UDP ─────────────────────────────────────────────────────────────
    __u32 inner_ip_len = inner_iph->ihl * 4;
    if (inner_ip_len < 20 || inner_ip_len > 60)
        return XDP_PASS;

    struct udphdr *udp = (void *)inner_iph + inner_ip_len;
    if ((void *)(udp + 1) > data_end)
        return XDP_PASS;

    if (udp->dest != config->target_port)
        return XDP_PASS;

    // ── Match — redirect whole GRE frame to AF_XDP (zero-copy on ENA) ─────────
    // PacketReplicator::extractUdpPayloadGre() strips headers in userspace.
    __u32 queue_idx = ctx->rx_queue_index;
    return bpf_redirect_map(&xsks_map, queue_idx, XDP_DROP);
}
