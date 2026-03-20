#include "dataplane.h"
#include "group_table.h"
#include "rewrite.h"
#include "stats.h"

#include <string.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_log.h>
#include <rte_cycles.h>

#define RTE_LOGTYPE_M2U RTE_LOGTYPE_USER1
#define DRAIN_INTERVAL_US 100
#define TX_BURST_MAX (MAX_SUBSCRIBERS_PER_GROUP * MAX_PKT_BURST)

extern volatile int force_quit;

struct tx_lcore_buf {
	uint16_t count;
	struct rte_mbuf *pkts[TX_BURST_MAX];
};

static inline void
flush_tx(struct tx_lcore_buf *buf, uint16_t port, uint16_t queue)
{
	if (buf->count == 0)
		return;

	/* Debug: log first packet details before TX */
	struct rte_mbuf *first = buf->pkts[0];
	RTE_LOG(DEBUG, M2U,
		"[tx] flushing %u pkts to port %u "
		"(pkt0: pkt_len=%u nb_segs=%u data_len=%u)\n",
		buf->count, port,
		first->pkt_len, first->nb_segs, first->data_len);

	uint16_t sent = rte_eth_tx_burst(port, queue, buf->pkts, buf->count);
	if (sent < buf->count)
		RTE_LOG(WARNING, M2U,
			"[tx] TX incomplete: sent %u/%u on port %u\n",
			sent, buf->count, port);
	else
		RTE_LOG(DEBUG, M2U, "[tx] sent %u/%u on port %u\n",
			sent, buf->count, port);

	for (uint16_t i = sent; i < buf->count; i++)
		rte_pktmbuf_free(buf->pkts[i]);
	buf->count = 0;
}

static inline void
enqueue_tx(struct tx_lcore_buf *buf, struct rte_mbuf **pkts, int n,
	   uint16_t port, uint16_t queue)
{
	for (int i = 0; i < n; i++) {
		buf->pkts[buf->count++] = pkts[i];
		if (buf->count >= MAX_PKT_BURST)
			flush_tx(buf, port, queue);
	}
}

static inline int
is_ipv4_multicast(const struct rte_ether_hdr *eth)
{
	/*
	 * Check IP-level multicast (224.0.0.0/4) instead of Ethernet
	 * multicast bit.  In cloud VPCs (AWS, GCP) the fabric rewrites
	 * multicast dst MACs to the ENI's unicast MAC, so the L2 check
	 * would miss legitimate multicast traffic arriving on an ENA port.
	 */
	if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		return 0;
	const struct rte_ipv4_hdr *ip = (const struct rte_ipv4_hdr *)
		((const char *)eth + sizeof(struct rte_ether_hdr));
	return (rte_be_to_cpu_32(ip->dst_addr) >> 28) == 0xE;
}

/*
 * Check if a unicast IPv4/UDP packet carries an m2u tunnel header.
 * If so, strip the tunnel header, rewrite headers to restore the original
 * multicast destination, and return the packet ready for TAP delivery.
 * Returns 0 on success (packet rewritten in-place), -1 if not m2u or error.
 */
static inline int
try_reverse_path(struct rte_mbuf *pkt)
{
	struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

	if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		return -1;

	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)
		((char *)eth + sizeof(struct rte_ether_hdr));

	if (ip->next_proto_id != IPPROTO_UDP)
		return -1;

	uint16_t ip_hdr_len = (uint16_t)((ip->version_ihl & 0x0f) * 4);
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)
		((char *)ip + ip_hdr_len);

	/* Check that we have enough data for UDP header + tunnel header */
	uint16_t udp_len = rte_be_to_cpu_16(udp->dgram_len);
	if (udp_len < sizeof(struct rte_udp_hdr) + sizeof(struct m2u_tunnel_hdr))
		return -1;

	/* Check for m2u tunnel magic */
	struct m2u_tunnel_hdr *thdr = (struct m2u_tunnel_hdr *)
		((char *)udp + sizeof(struct rte_udp_hdr));

	if (thdr->magic != rte_cpu_to_be_32(M2U_TUNNEL_MAGIC))
		return -1;

	/* Found m2u encapsulated packet — extract group_ip and strip tunnel */
	uint32_t group_ip = thdr->group_ip; /* already network byte order */

	RTE_LOG(DEBUG, M2U, "[reverse] m2u packet detected, restoring multicast\n");

	/* Remove the 8-byte tunnel header by shifting UDP payload left.
	 * We use memmove on the tunnel header area. */
	uint16_t payload_after_tunnel = udp_len - sizeof(struct rte_udp_hdr)
					- sizeof(struct m2u_tunnel_hdr);
	if (payload_after_tunnel > 0) {
		memmove((char *)thdr,
			(char *)thdr + sizeof(struct m2u_tunnel_hdr),
			payload_after_tunnel);
	}

	/* Trim packet by tunnel header size */
	if (rte_pktmbuf_trim(pkt, sizeof(struct m2u_tunnel_hdr)) != 0)
		return -1;

	/* Adjust IP total_length */
	uint16_t orig_ip_total = rte_be_to_cpu_16(ip->total_length);
	ip->total_length = rte_cpu_to_be_16(
		orig_ip_total - sizeof(struct m2u_tunnel_hdr));

	/* Adjust UDP dgram_len */
	udp->dgram_len = rte_cpu_to_be_16(
		udp_len - sizeof(struct m2u_tunnel_hdr));

	/* Rewrite IP dst to original multicast group */
	ip->dst_addr = group_ip;

	/* Recalculate IP checksum */
	ip->hdr_checksum = 0;
	ip->hdr_checksum = rte_ipv4_cksum(ip);

	/* Zero UDP checksum (pseudo-header changed) */
	udp->dgram_cksum = 0;

	/* Rewrite Ethernet dst to multicast MAC: 01:00:5e:xx:xx:xx
	 * Low 23 bits of group IP map to MAC bytes [3..5] */
	uint32_t gip_h = rte_be_to_cpu_32(group_ip);
	eth->dst_addr.addr_bytes[0] = 0x01;
	eth->dst_addr.addr_bytes[1] = 0x00;
	eth->dst_addr.addr_bytes[2] = 0x5e;
	eth->dst_addr.addr_bytes[3] = (gip_h >> 16) & 0x7f;
	eth->dst_addr.addr_bytes[4] = (gip_h >> 8) & 0xff;
	eth->dst_addr.addr_bytes[5] = gip_h & 0xff;

	return 0;
}

/*
 * Process a burst of packets from any RX source.
 * Classifies: multicast UDP -> rewrite, IGMP -> control plane, else drop.
 */
static inline void
process_rx_burst(struct rte_mbuf **pkts, uint16_t nb_rx,
		 struct rte_ring *ctrl_ring,
		 struct tx_lcore_buf *tx_buf,
		 const struct rte_ether_addr *tx_mac,
		 uint16_t tx_port,
		 int tx_is_tap,
		 struct tx_lcore_buf *tap_tx_buf,
		 const struct rte_ether_addr *tap_mac,
		 uint16_t tap_port)
{
	struct rte_mbuf *tx_out[TX_BURST_MAX];

	for (uint16_t i = 0; i < nb_rx; i++) {
		struct rte_ether_hdr *eth = rte_pktmbuf_mtod(
			pkts[i], struct rte_ether_hdr *);

		if (!is_ipv4_multicast(eth)) {
			/* Check for reverse path: m2u-encapsulated unicast */
			if (tap_tx_buf != NULL &&
			    try_reverse_path(pkts[i]) == 0) {
				/* Rewritten to multicast — set src/dst MAC
				 * for TAP delivery */
				eth = rte_pktmbuf_mtod(
					pkts[i], struct rte_ether_hdr *);
				rte_ether_addr_copy(tap_mac,
						    &eth->src_addr);
				tap_tx_buf->pkts[tap_tx_buf->count++] =
					pkts[i];
				if (tap_tx_buf->count >= MAX_PKT_BURST)
					flush_tx(tap_tx_buf, tap_port, 0);
				continue;
			}
			rte_pktmbuf_free(pkts[i]);
			continue;
		}

		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)
			((char *)eth + sizeof(struct rte_ether_hdr));

		if (ip->next_proto_id == IPPROTO_IGMP) {
			atomic_fetch_add(&g_stats.igmp_packets, 1);
			if (rte_ring_enqueue(ctrl_ring, pkts[i]) < 0)
				rte_pktmbuf_free(pkts[i]);
			continue;
		}

		if (ip->next_proto_id != IPPROTO_UDP) {
			rte_pktmbuf_free(pkts[i]);
			continue;
		}

		const struct subscriber_list *subs =
			group_table_lookup(ip->dst_addr);

		if (subs == NULL || subs->count == 0) {
			rte_pktmbuf_free(pkts[i]);
			continue;
		}

		int nb_out = rewrite_mcast_to_ucast(
			pkts[i], subs, tx_out, TX_BURST_MAX);

		/*
		 * Fix MACs for the TX port:
		 * - src MAC = TX port's MAC (required for AWS VPC anti-spoofing)
		 * - dst MAC = TX port's MAC when TX is a TAP/TUN, so the kernel
		 *   accepts the frame (otherwise dropped as PACKET_OTHERHOST).
		 *   The kernel then routes it and resolves the real dst MAC via ARP.
		 */
		for (int j = 0; j < nb_out; j++) {
			struct rte_ether_hdr *out_eth = rte_pktmbuf_mtod(
				tx_out[j], struct rte_ether_hdr *);
			rte_ether_addr_copy(tx_mac, &out_eth->src_addr);
			if (tx_is_tap)
				rte_ether_addr_copy(tx_mac, &out_eth->dst_addr);
		}

		enqueue_tx(tx_buf, tx_out, nb_out, tx_port, 0);
	}
}

int
dataplane_loop(void *arg)
{
	struct dataplane_args *args = arg;
	struct app_config *cfg = args->cfg;
	struct rte_ring *ctrl_ring = args->ctrl_ring;
	uint16_t rx_port = cfg->rx_port_id;
	uint16_t tx_port = cfg->tx_port_id;
	uint16_t tap_port = cfg->tap_port_id;
	int use_tap = cfg->use_tap;
	struct rte_mbuf *pkts[MAX_PKT_BURST];
	struct rte_mbuf *tap_pkts[MAX_PKT_BURST];
	struct tx_lcore_buf nic_tx = { .count = 0 };
	struct tx_lcore_buf tap_tx = { .count = 0 };
	struct rte_ether_addr tx_mac;
	struct rte_ether_addr tap_mac;
	uint64_t drain_tsc;
	uint64_t prev_tsc = 0;
	unsigned int lcore_id = rte_lcore_id();

	/* Get the TX port's MAC — needed for correct source MAC in output */
	if (rte_eth_macaddr_get(tx_port, &tx_mac) != 0) {
		RTE_LOG(ERR, M2U, "Cannot get MAC for TX port %u\n", tx_port);
		memset(&tx_mac, 0, sizeof(tx_mac));
	}

	/* Get the TAP port's MAC — needed for reverse path delivery */
	memset(&tap_mac, 0, sizeof(tap_mac));
	if (use_tap && rte_eth_macaddr_get(tap_port, &tap_mac) != 0)
		RTE_LOG(ERR, M2U, "Cannot get MAC for TAP port %u\n", tap_port);

	/*
	 * Detect if TX port is a TAP device. When sending via TAP, the
	 * kernel processes the frame — we must set dst MAC to the TAP's
	 * own MAC so the kernel accepts it and routes via the IP stack.
	 * When TX is a real NIC (ENA), use the subscriber's actual dst MAC.
	 */
	int tx_is_tap = 0;
	{
		struct rte_eth_dev_info tx_info;
		if (rte_eth_dev_info_get(tx_port, &tx_info) == 0 &&
		    tx_info.driver_name != NULL &&
		    strstr(tx_info.driver_name, "tap") != NULL)
			tx_is_tap = 1;
	}
	RTE_LOG(INFO, M2U, "TX port %u is_tap=%d\n", tx_port, tx_is_tap);

	drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
		    DRAIN_INTERVAL_US;

	RTE_LOG(INFO, M2U,
		"Data plane running on lcore %u (rx=%u tx=%u tap=%u)\n",
		lcore_id, rx_port, tx_port, use_tap ? tap_port : 0xFFFF);

	uint64_t stats_tsc = 0;
	uint64_t stats_interval = rte_get_tsc_hz() * 2; /* every 2 sec */

	while (!force_quit) {
		uint64_t cur_tsc = rte_rdtsc();

		/* Periodic TX drain + stats */
		if (cur_tsc - prev_tsc > drain_tsc) {
			flush_tx(&nic_tx, tx_port, 0);
			if (use_tap)
				flush_tx(&tap_tx, tap_port, 0);
			prev_tsc = cur_tsc;
		}
		if (cur_tsc - stats_tsc > stats_interval) {
			uint64_t rx = atomic_load(&g_stats.rx_packets);
			if (rx > 0 || stats_tsc == 0)
				RTE_LOG(INFO, M2U,
					"[dp] rx=%lu mcast=%lu tx=%lu drop=%lu\n",
					rx,
					atomic_load(&g_stats.mcast_packets),
					atomic_load(&g_stats.tx_packets),
					atomic_load(&g_stats.drop_packets));
			stats_tsc = cur_tsc;
		}

		/* ---- RX from primary port ---- */
		uint16_t nb_rx = rte_eth_rx_burst(rx_port, 0, pkts,
						  MAX_PKT_BURST);
		if (nb_rx > 0) {
			atomic_fetch_add(&g_stats.rx_packets, nb_rx);
			uint16_t before_nic = nic_tx.count;
			uint16_t before_tap = tap_tx.count;
			process_rx_burst(pkts, nb_rx, ctrl_ring,
					 &nic_tx, &tx_mac, tx_port,
					 tx_is_tap,
					 use_tap ? &tap_tx : NULL,
					 &tap_mac, tap_port);
			uint16_t produced = (nic_tx.count - before_nic) +
					    (tap_tx.count - before_tap);
			if (produced > 0) {
				atomic_fetch_add(&g_stats.tx_packets, produced);
				atomic_fetch_add(&g_stats.mcast_packets, nb_rx);
				/* Flush immediately for low latency */
				flush_tx(&nic_tx, tx_port, 0);
				if (use_tap)
					flush_tx(&tap_tx, tap_port, 0);
			} else {
				atomic_fetch_add(&g_stats.drop_packets, nb_rx);
			}
		}

		/* ---- RX from TAP (multicast + IGMP from local apps) ---- */
		if (use_tap) {
			uint16_t nb_tap = rte_eth_rx_burst(tap_port, 0,
				tap_pkts, MAX_PKT_BURST);
			if (nb_tap > 0) {
				atomic_fetch_add(&g_stats.rx_packets, nb_tap);
				uint16_t before = nic_tx.count;
				/* No reverse path for TAP RX — these are local */
				process_rx_burst(tap_pkts, nb_tap, ctrl_ring,
						 &nic_tx, &tx_mac, tx_port,
						 tx_is_tap,
						 NULL, NULL, 0);
				uint16_t produced = nic_tx.count - before;
				if (produced > 0) {
					atomic_fetch_add(&g_stats.tx_packets, produced);
					atomic_fetch_add(&g_stats.mcast_packets, nb_tap);
					/* Flush immediately for low latency */
					flush_tx(&nic_tx, tx_port, 0);
				} else {
					atomic_fetch_add(&g_stats.drop_packets, nb_tap);
				}
			}
		}
	}

	flush_tx(&nic_tx, tx_port, 0);
	if (use_tap)
		flush_tx(&tap_tx, tap_port, 0);

	RTE_LOG(INFO, M2U, "Data plane lcore %u exiting\n", lcore_id);
	return 0;
}
