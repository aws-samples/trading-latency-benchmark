/*
 * subscriber_filter.c — XDP program for the latency_receiver.
 *
 * Redirects GRE (IP proto 47) unicast packets directly into an AF_XDP
 * socket, bypassing the kernel IP stack entirely.  All other traffic
 * is passed to the kernel as normal (SSH, ARP, etc. unaffected).
 *
 * Map: xsks_map[rx_queue_index] → AF_XDP socket fd.
 *      Populated by latency_receiver at startup.
 *
 * Fallback: XDP_PASS — if no socket registered for this queue, packet
 *           falls through to the kernel SOCK_RAW path as before.
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <linux/if_ether.h>
#include <linux/ip.h>

struct {
	__uint(type,       BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, 64);
	__type(key,   __u32);
	__type(value, __u32);
} xsks_map SEC(".maps");

SEC("xdp")
int xdp_gre_redirect(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;

	struct ethhdr *eth = data;
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;
	if (eth->h_proto != __constant_htons(ETH_P_IP))
		return XDP_PASS;

	struct iphdr *ip = (void *)(eth + 1);
	if ((void *)(ip + 1) > data_end)
		return XDP_PASS;
	if (ip->protocol != 47)  /* IPPROTO_GRE */
		return XDP_PASS;

	return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
