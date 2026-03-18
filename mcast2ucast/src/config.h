#ifndef MCAST2UCAST_CONFIG_H
#define MCAST2UCAST_CONFIG_H

#include <stdint.h>
#include <rte_ether.h>

#define MAX_PRODUCERS            64
#define MAX_STATIC_SUBS          256
#define MAX_GROUPS               1024
#define MAX_SUBSCRIBERS_PER_GROUP 128
#define MAX_PKT_BURST            32
#define RX_RING_SIZE             1024
#define TX_RING_SIZE             1024
#define CTRL_RING_SIZE           256
#define NB_PACKET_MBUF           8192
#define NB_HEADER_MBUF           8192
#define NB_CLONE_MBUF            8192
#define MEMPOOL_CACHE_SIZE       256

struct producer_entry {
	uint32_t group_ip;      /* network byte order */
	uint32_t producer_ip;   /* network byte order */
	uint16_t producer_port; /* host byte order */
};

struct static_sub_entry {
	uint32_t group_ip;      /* network byte order */
	uint32_t unicast_ip;    /* network byte order */
	uint16_t unicast_port;  /* host byte order */
	struct rte_ether_addr dst_mac;
	int has_mac;
};

struct app_config {
	/* Port IDs */
	uint16_t rx_port_id;
	uint16_t tx_port_id;
	int      use_tap;
	char     tap_name[32];
	uint16_t tap_port_id;   /* filled in at runtime */

	/* Our identity */
	uint32_t local_ip;      /* network byte order */
	uint16_t ctrl_port;     /* host byte order, for producer notifications */

	/* Producer map */
	struct producer_entry producers[MAX_PRODUCERS];
	int nb_producers;

	/* Static subscribers */
	struct static_sub_entry static_subs[MAX_STATIC_SUBS];
	int nb_static_subs;

	/* Config file path */
	char config_file[256];
};

int config_parse_args(int argc, char **argv, struct app_config *cfg);
int config_load_file(struct app_config *cfg);

const struct producer_entry *config_find_producer(const struct app_config *cfg,
						  uint32_t group_ip);

#endif
