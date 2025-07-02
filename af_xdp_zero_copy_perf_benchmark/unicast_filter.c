// unicast_filter.c - XDP program for unicast UDP packet filtering and forwarding
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
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

// Required for logging in XDP programs
#define DEBUG 1
#define bpf_debug(fmt, ...)                 \
    ({                                      \
        if (DEBUG)                          \
            bpf_printk(fmt, ##__VA_ARGS__); \
    })

// Required license for BPF programs
char _license[] SEC("license") = "GPL";

// Statistics map for monitoring
struct
{
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} stats SEC(".maps");

// XSK map for socket redirection
struct
{
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 256);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

// Configuration map for target IP and port
struct unicast_config {
    __u32 target_ip;    // Target IP address in network byte order
    __u16 target_port;  // Target port in network byte order
    __u16 padding;      // Padding for alignment
};

struct
{
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
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
int unicast_filter(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    // Count all packets
    increment_counter(0);

    // Ensure we have enough data for an Ethernet header
    if (data + sizeof(struct ethhdr) > data_end)
    {
        // bpf_debug("XDP: Packet too short for Ethernet header");
        return XDP_PASS;
    }

    struct ethhdr *eth = data;

    // Check for IPv4 packets
    if (eth->h_proto != bpf_htons(ETH_P_IP))
    {
        return XDP_PASS;
    }

    // IPv4 packet found
    increment_counter(1);

    // Access IPv4 header with proper bounds checking
    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
    {
        // bpf_debug("XDP: Packet too short for IP header");
        return XDP_PASS;
    }

    // Check for UDP protocol - we only process UDP
    if (iph->protocol != IPPROTO_UDP)
    {
        return XDP_PASS;
    }

    // UDP packet found
    increment_counter(2);

    // Safely calculate UDP header position
    __u32 ip_hdr_size = iph->ihl * 4;
    if (ip_hdr_size < 20)
    {
        // bpf_debug("XDP: Invalid IP header size: %d", ip_hdr_size);
        return XDP_PASS;
    }

    void *udp_start = (void *)iph + ip_hdr_size;
    if (udp_start + sizeof(struct udphdr) > data_end)
    {
        // bpf_debug("XDP: Packet too short for UDP header");
        return XDP_PASS;
    }

    struct udphdr *udp = (struct udphdr *)udp_start;

    // Get configuration for target IP and port
    __u32 config_key = 0;
    struct unicast_config *config = bpf_map_lookup_elem(&config_map, &config_key);
    if (!config)
    {
        // No configuration found, pass all UDP packets
        // bpf_debug("XDP: No configuration found, passing packet");
        return XDP_PASS;
    }

    // Check if this packet matches our target IP and port
    __u16 dst_port = udp->dest;  // Already in network byte order
    __u32 dst_ip = iph->daddr;   // Already in network byte order

    if (dst_ip != config->target_ip || dst_port != config->target_port)
    {
        // Not our target packet, pass it through
        return XDP_PASS;
    }

    // Target packet found - log and redirect
    increment_counter(3);

    __u8 *saddr = (__u8 *)&iph->saddr;
    __u8 *daddr = (__u8 *)&iph->daddr;
    __u16 src_port = bpf_ntohs(udp->source);
    __u16 target_port_host = bpf_ntohs(config->target_port);

    // bpf_debug("XDP: Target UDP packet %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d",
              saddr[0], saddr[1], saddr[2], saddr[3], src_port,
              daddr[0], daddr[1], daddr[2], daddr[3], target_port_host);

    // Get the actual queue index from the context
    __u32 queue_idx = ctx->rx_queue_index;
    int *fd_ptr = bpf_map_lookup_elem(&xsks_map, &queue_idx);
    if (!fd_ptr)
    {
        // bpf_debug("XDP: No AF_XDP socket found for queue %d", queue_idx);
        return XDP_PASS;
    }

    // Redirect packet to the AF_XDP socket for zero-copy processing
    // bpf_debug("XDP: Redirecting target UDP packet on queue %d to socket %d",
              queue_idx, *fd_ptr);

    increment_counter(4);
    return bpf_redirect_map(&xsks_map, queue_idx, XDP_DROP);
}
