// multicast_filter.c - XDP program for multicast UDP packet filtering and forwarding
/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

char _license[] SEC("license") = "GPL";

// Statistics map for monitoring
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} stats SEC(".maps");

// XSK map for socket redirection — queue index -> AF_XDP socket fd
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 256);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

// Configuration: which multicast group + port to intercept
struct unicast_config {
    __u32 target_ip;    // Multicast group address in network byte order
    __u16 target_port;  // Target port in network byte order
    __u16 padding;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct unicast_config);
} config_map SEC(".maps");

SEC("xdp")
int multicast_filter(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data     = (void *)(long)ctx->data;

    if (data + sizeof(struct ethhdr) > data_end)
        return XDP_PASS;

    struct ethhdr *eth = data;
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_PASS;

    if (iph->protocol != IPPROTO_UDP)
        return XDP_PASS;

    // Fast-path rejection: must be in 224.0.0.0/4 multicast range.
    // bpf_ntohl converts the NBO daddr to host order so we can test the
    // top nibble with a plain arithmetic mask.
    if ((bpf_ntohl(iph->daddr) & 0xF0000000) != 0xE0000000)
        return XDP_PASS;

    __u32 ip_hdr_size = iph->ihl * 4;
    if (ip_hdr_size < 20)
        return XDP_PASS;

    void *udp_start = (void *)iph + ip_hdr_size;
    if (udp_start + sizeof(struct udphdr) > data_end)
        return XDP_PASS;

    struct udphdr *udp = (struct udphdr *)udp_start;

    // Look up configured multicast group + port
    __u32 key = 0;
    struct unicast_config *config = bpf_map_lookup_elem(&config_map, &key);
    if (!config)
        return XDP_PASS;   // No config yet — pass all traffic normally

    // Match specific group address and port
    if (iph->daddr != config->target_ip || udp->dest != config->target_port)
        return XDP_PASS;

    // Redirect to the AF_XDP socket registered for this RX queue.
    // XDP_DROP is the fallback if no socket is registered for the queue yet.
    __u32 queue_idx = ctx->rx_queue_index;
    return bpf_redirect_map(&xsks_map, queue_idx, XDP_DROP);
}
