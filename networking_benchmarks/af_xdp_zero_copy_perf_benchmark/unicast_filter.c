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
#define DEBUG 0
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

// Maximum number of listen addresses a single packet_replicator instance can intercept.
// config_map slots may be sparse (dynamic add/remove); target_ip == 0 marks an unused
// slot — the scan always checks all MAX_GROUPS entries via continue, not break.
#define MAX_GROUPS 16

// Configuration map for target IP and port
struct unicast_config {
    __u32 target_ip;    // Target IP address in network byte order (0 = unused slot)
    __u16 target_port;  // Target port in network byte order
    __u16 padding;      // Padding for alignment
};

struct
{
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
int unicast_filter(struct xdp_md *ctx)
{
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    // Count all packets
    // increment_counter(0);

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
    // increment_counter(1);

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
    // increment_counter(2);

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

    // Scan up to MAX_GROUPS config entries.  Entries are populated sequentially;
    // target_ip == 0 means the slot is unused.
    __u8 matched = 0;
    #pragma unroll
    for (int _idx = 0; _idx < MAX_GROUPS; _idx++) {
        if (!matched) {
            __u32 _k = (__u32)_idx;
            struct unicast_config *cfg = bpf_map_lookup_elem(&config_map, &_k);
            if (!cfg || cfg->target_ip == 0)
                continue;
            if (iph->daddr == cfg->target_ip && udp->dest == cfg->target_port)
                matched = 1;
        }
    }

    if (!matched)
        return XDP_PASS;

    // Redirect to the AF_XDP socket registered for this RX queue.
    // XDP_PASS fallback: if no socket is registered (e.g. during the brief window
    // between loadXdpProgram and registerXskMap), packets fall through to the kernel
    // stack rather than being silently dropped.  This also avoids the extra
    // bpf_map_lookup_elem that the old explicit-check pattern incurred per packet.
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}
