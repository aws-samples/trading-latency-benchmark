#ifndef MCAST2UCAST_GROUP_TABLE_H
#define MCAST2UCAST_GROUP_TABLE_H

#include <stdint.h>
#include <rte_ether.h>
#include "config.h"

struct subscriber {
	uint32_t unicast_ip;            /* network byte order */
	uint16_t unicast_port;          /* network byte order */
	struct rte_ether_addr dst_mac;
	uint16_t tx_port;               /* DPDK port ID for egress */
	uint8_t  is_static;             /* 1 = from config, 0 = dynamic (IGMP/ctrl) */
	uint8_t  is_local;              /* 1 = local IGMP join, 0 = remote subscriber */
	time_t   last_seen;             /* last keepalive/join timestamp (0 = static) */
};

struct subscriber_list {
	uint32_t count;
	struct subscriber entries[MAX_SUBSCRIBERS_PER_GROUP];
};

int  group_table_init(void);
void group_table_destroy(void);

/* Lock-free lookup for data plane — returns NULL if group not found */
const struct subscriber_list *group_table_lookup(uint32_t group_ip);

/* Control plane mutation */
int group_table_add_subscriber(uint32_t group_ip, const struct subscriber *sub);
int group_table_remove_subscriber(uint32_t group_ip,
				  uint32_t unicast_ip, uint16_t unicast_port);
int group_table_remove_group(uint32_t group_ip);

void group_table_dump(void);

/* Remove all dynamic subscribers older than max_age_sec. Returns count removed. */
int group_table_expire_subscribers(int max_age_sec);

/* Refresh the last_seen timestamp for a matching subscriber. Returns 0 on hit. */
int group_table_refresh_subscriber(uint32_t group_ip,
				   uint32_t unicast_ip, uint16_t unicast_port);

/* Iterate all groups. Callback receives group IP (NBO), subscriber list, and
 * user context.  Return non-zero from cb to stop early. */
typedef int (*group_table_iter_cb)(uint32_t group_ip,
				   const struct subscriber_list *subs,
				   void *ctx);
int group_table_iterate(group_table_iter_cb cb, void *ctx);

#endif
