#include "rewrite.h"

#include <string.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_malloc.h>
#include <rte_log.h>

#define RTE_LOGTYPE_M2U RTE_LOGTYPE_USER1

/*
 * Header mbuf size: Eth(14) + IP(20) + UDP(8) + m2u_tunnel_hdr(8) = 50 bytes.
 * Plenty of room in 2 * RTE_PKTMBUF_HEADROOM.
 */
#define HDR_MBUF_DATA_SIZE (2 * RTE_PKTMBUF_HEADROOM)

static struct rte_mempool *packet_pool;
static struct rte_mempool *header_pool;
static struct rte_mempool *clone_pool;

struct rte_mempool *
rewrite_get_packet_pool(void)
{
	return packet_pool;
}

int
rewrite_init(unsigned int socket_id)
{
	packet_pool = rte_pktmbuf_pool_create("packet_pool",
		NB_PACKET_MBUF, MEMPOOL_CACHE_SIZE, 0,
		RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
	if (packet_pool == NULL) {
		RTE_LOG(ERR, M2U, "Cannot create packet mempool\n");
		return -1;
	}

	header_pool = rte_pktmbuf_pool_create("header_pool",
		NB_HEADER_MBUF, MEMPOOL_CACHE_SIZE, 0,
		HDR_MBUF_DATA_SIZE, socket_id);
	if (header_pool == NULL) {
		RTE_LOG(ERR, M2U, "Cannot create header mempool\n");
		return -1;
	}

	/* Clone pool: data_room_size=0, these mbufs are indirect (metadata only) */
	clone_pool = rte_pktmbuf_pool_create("clone_pool",
		NB_CLONE_MBUF, MEMPOOL_CACHE_SIZE, 0,
		0, socket_id);
	if (clone_pool == NULL) {
		RTE_LOG(ERR, M2U, "Cannot create clone mempool\n");
		return -1;
	}

	RTE_LOG(INFO, M2U,
		"Rewrite engine initialized: packet=%u header=%u clone=%u\n",
		NB_PACKET_MBUF, NB_HEADER_MBUF, NB_CLONE_MBUF);
	return 0;
}

void
rewrite_cleanup(void)
{
	rte_mempool_free(clone_pool);
	rte_mempool_free(header_pool);
	rte_mempool_free(packet_pool);
	clone_pool = NULL;
	header_pool = NULL;
	packet_pool = NULL;
}

/*
 * Build a unicast packet for one subscriber.
 *
 * The header mbuf gets: Eth + IP + UDP + m2u_tunnel_hdr (50 bytes).
 * The data mbuf is either a clone or refcnt-shared, starting at the
 * original UDP payload (after the UDP header).
 *
 * What gets copied:    Eth(14) + IP(20) + UDP(8) + tunnel(8) = 50 bytes
 * What is zero-copy:   UDP payload (potentially kilobytes)
 */
static struct rte_mbuf *
build_unicast_pkt(struct rte_mbuf *data, int use_clone,
		  const struct rte_ipv4_hdr *orig_ip,
		  const struct rte_udp_hdr *orig_udp,
		  uint32_t group_ip,
		  const struct subscriber *sub,
		  const struct rte_ether_addr *src_mac)
{
	struct rte_mbuf *hdr;
	struct rte_mbuf *payload;

	hdr = rte_pktmbuf_alloc(header_pool);
	if (hdr == NULL)
		return NULL;

	if (use_clone) {
		payload = rte_pktmbuf_clone(data, clone_pool);
		if (payload == NULL) {
			rte_pktmbuf_free(hdr);
			return NULL;
		}
	} else {
		payload = data;
	}

	/* Write Ethernet header */
	struct rte_ether_hdr *eth = (struct rte_ether_hdr *)
		rte_pktmbuf_append(hdr, sizeof(struct rte_ether_hdr));
	if (eth == NULL)
		goto fail;
	rte_ether_addr_copy(&sub->dst_mac, &eth->dst_addr);
	rte_ether_addr_copy(src_mac, &eth->src_addr);
	eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	/* Write IP header — copy from original, rewrite destination.
	 * Adjust total_length for the added tunnel header. */
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)
		rte_pktmbuf_append(hdr, sizeof(struct rte_ipv4_hdr));
	if (ip == NULL)
		goto fail;
	*ip = *orig_ip;
	ip->dst_addr = sub->unicast_ip;
	uint16_t orig_total = rte_be_to_cpu_16(orig_ip->total_length);
	ip->total_length = rte_cpu_to_be_16(
		orig_total + sizeof(struct m2u_tunnel_hdr));
	ip->hdr_checksum = 0;
	ip->hdr_checksum = rte_ipv4_cksum(ip);

	/* Write UDP header — copy from original, adjust length */
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)
		rte_pktmbuf_append(hdr, sizeof(struct rte_udp_hdr));
	if (udp == NULL)
		goto fail;
	*udp = *orig_udp;
	uint16_t orig_udp_len = rte_be_to_cpu_16(orig_udp->dgram_len);
	udp->dgram_len = rte_cpu_to_be_16(
		orig_udp_len + sizeof(struct m2u_tunnel_hdr));
	udp->dgram_cksum = 0;

	/* Write m2u tunnel header */
	struct m2u_tunnel_hdr *thdr = (struct m2u_tunnel_hdr *)
		rte_pktmbuf_append(hdr, sizeof(struct m2u_tunnel_hdr));
	if (thdr == NULL)
		goto fail;
	thdr->magic = rte_cpu_to_be_32(M2U_TUNNEL_MAGIC);
	thdr->group_ip = group_ip;

	/* Chain: hdr -> payload (UDP payload only, zero-copy) */
	hdr->next = payload;
	hdr->pkt_len = hdr->data_len + payload->pkt_len;
	hdr->nb_segs = (uint16_t)(payload->nb_segs + 1);

	return hdr;

fail:
	if (use_clone)
		rte_pktmbuf_free(payload);
	rte_pktmbuf_free(hdr);
	return NULL;
}

int
rewrite_mcast_to_ucast(struct rte_mbuf *pkt,
			const struct subscriber_list *subs,
			struct rte_mbuf **tx_pkts,
			int max_tx_pkts)
{
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr orig_ip;
	struct rte_udp_hdr orig_udp;
	struct rte_ether_addr src_mac;
	uint16_t ip_hdr_len;
	uint32_t group_ip;
	int n = (int)subs->count;
	int use_clone;
	int nb_tx = 0;
	int pkt_consumed = 0;

	if (n == 0 || n > max_tx_pkts) {
		rte_pktmbuf_free(pkt);
		return 0;
	}

	/* Save the source MAC from the incoming frame */
	eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
	rte_ether_addr_copy(&eth_hdr->src_addr, &src_mac);

	/* Save the original IP header before stripping */
	struct rte_ipv4_hdr *ip_ptr = (struct rte_ipv4_hdr *)
		((char *)eth_hdr + sizeof(struct rte_ether_hdr));
	ip_hdr_len = (uint16_t)((ip_ptr->version_ihl & 0x0f) * 4);
	orig_ip = *ip_ptr;
	group_ip = ip_ptr->dst_addr; /* save multicast group IP */

	/* Save the original UDP header */
	struct rte_udp_hdr *udp_ptr = (struct rte_udp_hdr *)
		((char *)ip_ptr + ip_hdr_len);
	orig_udp = *udp_ptr;

	/* Strip Ethernet + IP + UDP headers — data now starts at UDP payload */
	if (rte_pktmbuf_adj(pkt, (uint16_t)(sizeof(struct rte_ether_hdr) +
					     ip_hdr_len +
					     sizeof(struct rte_udp_hdr))) == NULL) {
		rte_pktmbuf_free(pkt);
		return 0;
	}

	/* Strategy selection (from examples/ipv4_multicast) */
	use_clone = (n <= MCAST_CLONE_PORTS && pkt->nb_segs <= MCAST_CLONE_SEGS);

	if (!use_clone)
		rte_pktmbuf_refcnt_update(pkt, (uint16_t)n);

	for (int i = 0; i < n; i++) {
		struct rte_mbuf *out;

		if (use_clone && i == n - 1) {
			out = build_unicast_pkt(pkt, 0,
						&orig_ip, &orig_udp, group_ip,
						&subs->entries[i], &src_mac);
			if (out != NULL) {
				tx_pkts[nb_tx++] = out;
				pkt_consumed = 1;
			}
		} else {
			out = build_unicast_pkt(pkt, use_clone,
						&orig_ip, &orig_udp, group_ip,
						&subs->entries[i], &src_mac);
			if (out != NULL)
				tx_pkts[nb_tx++] = out;
			else if (!use_clone)
				rte_pktmbuf_free(pkt);
		}
	}

	if (!use_clone || !pkt_consumed)
		rte_pktmbuf_free(pkt);

	return nb_tx;
}
