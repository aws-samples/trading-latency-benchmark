#ifndef MCAST2UCAST_REWRITE_H
#define MCAST2UCAST_REWRITE_H

#include <rte_mbuf.h>
#include "group_table.h"

/* Threshold for choosing clone vs refcnt strategy */
#define MCAST_CLONE_PORTS 2
#define MCAST_CLONE_SEGS  2

/*
 * Lightweight tunnel header prepended to the UDP payload when forwarding
 * multicast-as-unicast between hosts.  The subscriber uses the magic to
 * detect encapsulated packets and the group_ip to reconstruct the original
 * multicast destination before delivering to local apps via TAP.
 *
 * Wire format (after Eth + IP + UDP headers):
 *   [m2u_tunnel_hdr (8 bytes)] [original UDP payload]
 */
#define M2U_TUNNEL_MAGIC 0x4D325501  /* "M2U" + version 1 */

struct __attribute__((packed)) m2u_tunnel_hdr {
	uint32_t magic;      /* M2U_TUNNEL_MAGIC */
	uint32_t group_ip;   /* original multicast group IP (network byte order) */
};

int  rewrite_init(unsigned int socket_id);
void rewrite_cleanup(void);

/*
 * Convert a multicast packet into multiple unicast packets.
 * The original packet is consumed (freed or ref-managed internally).
 * Produced packets are written to tx_pkts[0..return-1].
 * Returns the number of unicast packets produced.
 */
int rewrite_mcast_to_ucast(struct rte_mbuf *pkt,
			    const struct subscriber_list *subs,
			    struct rte_mbuf **tx_pkts,
			    int max_tx_pkts);

struct rte_mempool *rewrite_get_packet_pool(void);

#endif
