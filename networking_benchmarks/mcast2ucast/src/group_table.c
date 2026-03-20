#include "group_table.h"

#include <string.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>

#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_lcore.h>

#define RTE_LOGTYPE_M2U RTE_LOGTYPE_USER1

static struct rte_hash *group_hash;
static pthread_mutex_t  write_lock = PTHREAD_MUTEX_INITIALIZER;

int
group_table_init(void)
{
	struct rte_hash_parameters params = {
		.name       = "group_table",
		.entries    = MAX_GROUPS,
		.key_len    = sizeof(uint32_t),
		.hash_func  = rte_hash_crc,
		.socket_id  = rte_socket_id(),
		.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF,
	};

	group_hash = rte_hash_create(&params);
	if (group_hash == NULL) {
		RTE_LOG(ERR, M2U, "Cannot create group hash table\n");
		return -1;
	}

	RTE_LOG(INFO, M2U, "Group table initialized (max %d groups)\n",
		MAX_GROUPS);
	return 0;
}

void
group_table_destroy(void)
{
	uint32_t iter = 0;
	const void *key;
	void *data;

	if (group_hash == NULL)
		return;

	/* Free all subscriber lists */
	while (rte_hash_iterate(group_hash, &key, &data, &iter) >= 0)
		rte_free(data);

	rte_hash_free(group_hash);
	group_hash = NULL;
}

const struct subscriber_list *
group_table_lookup(uint32_t group_ip)
{
	void *data = NULL;
	int ret;

	ret = rte_hash_lookup_data(group_hash, &group_ip, &data);
	if (ret < 0)
		return NULL;

	return (const struct subscriber_list *)data;
}

int
group_table_add_subscriber(uint32_t group_ip, const struct subscriber *sub)
{
	struct subscriber_list *subs;
	struct subscriber_list *new_subs;
	void *data = NULL;
	int ret;

	pthread_mutex_lock(&write_lock);

	ret = rte_hash_lookup_data(group_hash, &group_ip, &data);
	subs = (ret >= 0) ? data : NULL;

	/* Check for duplicate */
	if (subs != NULL) {
		for (uint32_t i = 0; i < subs->count; i++) {
			if (subs->entries[i].unicast_ip == sub->unicast_ip &&
			    subs->entries[i].unicast_port == sub->unicast_port) {
				pthread_mutex_unlock(&write_lock);
				return 0; /* already exists */
			}
		}
		if (subs->count >= MAX_SUBSCRIBERS_PER_GROUP) {
			RTE_LOG(WARNING, M2U,
				"Group table full for group\n");
			pthread_mutex_unlock(&write_lock);
			return -1;
		}
	}

	/* Allocate a new subscriber list (copy-on-write) */
	new_subs = rte_zmalloc("sub_list", sizeof(*new_subs), 0);
	if (new_subs == NULL) {
		pthread_mutex_unlock(&write_lock);
		return -1;
	}

	if (subs != NULL)
		rte_memcpy(new_subs, subs, sizeof(*subs));

	new_subs->entries[new_subs->count] = *sub;
	new_subs->count++;

	/* Atomic pointer swap via rte_hash_add_key_data */
	ret = rte_hash_add_key_data(group_hash, &group_ip, new_subs);
	if (ret < 0) {
		rte_free(new_subs);
		pthread_mutex_unlock(&write_lock);
		return -1;
	}

	/*
	 * Free old list after a delay. In production, use RCU (rte_rcu_qsbr)
	 * to wait for all readers to quiesce. For now, a simple delay suffices
	 * because IGMP events are very infrequent vs. data-plane reads.
	 */
	if (subs != NULL) {
		rte_delay_us_sleep(1000);
		rte_free(subs);
	}

	{
		char buf[INET_ADDRSTRLEN];
		struct in_addr a = { .s_addr = group_ip };
		inet_ntop(AF_INET, &a, buf, sizeof(buf));
		RTE_LOG(INFO, M2U, "Added subscriber to group %s (%u total)\n",
			buf, new_subs->count);
	}

	pthread_mutex_unlock(&write_lock);
	return 0;
}

int
group_table_remove_subscriber(uint32_t group_ip,
			      uint32_t unicast_ip, uint16_t unicast_port)
{
	struct subscriber_list *subs;
	struct subscriber_list *new_subs;
	void *data = NULL;
	int ret;

	pthread_mutex_lock(&write_lock);

	ret = rte_hash_lookup_data(group_hash, &group_ip, &data);
	if (ret < 0) {
		pthread_mutex_unlock(&write_lock);
		return -1;
	}
	subs = data;

	/* Find the subscriber */
	uint32_t idx = UINT32_MAX;
	for (uint32_t i = 0; i < subs->count; i++) {
		if (subs->entries[i].unicast_ip == unicast_ip &&
		    subs->entries[i].unicast_port == unicast_port) {
			idx = i;
			break;
		}
	}
	if (idx == UINT32_MAX) {
		pthread_mutex_unlock(&write_lock);
		return -1;
	}

	if (subs->count == 1) {
		/* Last subscriber — remove the group entirely */
		rte_hash_del_key(group_hash, &group_ip);
		rte_delay_us_sleep(1000);
		rte_free(subs);
		pthread_mutex_unlock(&write_lock);
		return 0;
	}

	/* Build new list without this subscriber */
	new_subs = rte_zmalloc("sub_list", sizeof(*new_subs), 0);
	if (new_subs == NULL) {
		pthread_mutex_unlock(&write_lock);
		return -1;
	}
	rte_memcpy(new_subs, subs, sizeof(*subs));
	/* Compact: move last entry into the removed slot */
	new_subs->count--;
	if (idx < new_subs->count)
		new_subs->entries[idx] = new_subs->entries[new_subs->count];

	rte_hash_add_key_data(group_hash, &group_ip, new_subs);
	rte_delay_us_sleep(1000);
	rte_free(subs);

	pthread_mutex_unlock(&write_lock);
	return 0;
}

int
group_table_remove_group(uint32_t group_ip)
{
	void *data = NULL;
	int ret;

	pthread_mutex_lock(&write_lock);

	ret = rte_hash_lookup_data(group_hash, &group_ip, &data);
	if (ret >= 0) {
		rte_hash_del_key(group_hash, &group_ip);
		rte_delay_us_sleep(1000);
		rte_free(data);
	}

	pthread_mutex_unlock(&write_lock);
	return (ret >= 0) ? 0 : -1;
}

int
group_table_iterate(group_table_iter_cb cb, void *ctx)
{
	uint32_t iter = 0;
	const void *key;
	void *data;
	int count = 0;

	if (group_hash == NULL)
		return 0;

	while (rte_hash_iterate(group_hash, &key, &data, &iter) >= 0) {
		uint32_t gip = *(const uint32_t *)key;
		const struct subscriber_list *subs = data;
		count++;
		if (cb && cb(gip, subs, ctx) != 0)
			break;
	}
	return count;
}

int
group_table_refresh_subscriber(uint32_t group_ip,
			       uint32_t unicast_ip, uint16_t unicast_port)
{
	void *data = NULL;
	int ret;

	pthread_mutex_lock(&write_lock);

	ret = rte_hash_lookup_data(group_hash, &group_ip, &data);
	if (ret < 0) {
		pthread_mutex_unlock(&write_lock);
		return -1;
	}

	struct subscriber_list *subs = data;
	for (uint32_t i = 0; i < subs->count; i++) {
		if (subs->entries[i].unicast_ip == unicast_ip &&
		    subs->entries[i].unicast_port == unicast_port) {
			subs->entries[i].last_seen = time(NULL);
			pthread_mutex_unlock(&write_lock);
			return 0;
		}
	}

	pthread_mutex_unlock(&write_lock);
	return -1;
}

int
group_table_expire_subscribers(int max_age_sec)
{
	uint32_t iter = 0;
	const void *key;
	void *data;
	int total_removed = 0;
	time_t now = time(NULL);

	pthread_mutex_lock(&write_lock);

	while (rte_hash_iterate(group_hash, &key, &data, &iter) >= 0) {
		uint32_t gip = *(const uint32_t *)key;
		struct subscriber_list *subs = data;

		for (int i = (int)subs->count - 1; i >= 0; i--) {
			if (subs->entries[i].is_static)
				continue;
			if (subs->entries[i].last_seen == 0)
				continue;
			if (now - subs->entries[i].last_seen > max_age_sec) {
				char gbuf[INET_ADDRSTRLEN], sbuf[INET_ADDRSTRLEN];
				struct in_addr a;
				a.s_addr = gip;
				inet_ntop(AF_INET, &a, gbuf, sizeof(gbuf));
				a.s_addr = subs->entries[i].unicast_ip;
				inet_ntop(AF_INET, &a, sbuf, sizeof(sbuf));
				RTE_LOG(INFO, M2U,
					"Expiring subscriber %s from group %s "
					"(last_seen %lds ago)\n",
					sbuf, gbuf,
					(long)(now - subs->entries[i].last_seen));

				subs->count--;
				if ((uint32_t)i < subs->count)
					subs->entries[i] = subs->entries[subs->count];
				total_removed++;
			}
		}

		if (subs->count == 0) {
			rte_hash_del_key(group_hash, &gip);
			rte_free(subs);
		}
	}

	pthread_mutex_unlock(&write_lock);
	return total_removed;
}

void
group_table_dump(void)
{
	uint32_t iter = 0;
	const void *key;
	void *data;
	char grp_buf[INET_ADDRSTRLEN], sub_buf[INET_ADDRSTRLEN];

	RTE_LOG(INFO, M2U, "=== Group Table ===\n");
	while (rte_hash_iterate(group_hash, &key, &data, &iter) >= 0) {
		uint32_t gip = *(const uint32_t *)key;
		struct subscriber_list *subs = data;
		struct in_addr a = { .s_addr = gip };
		inet_ntop(AF_INET, &a, grp_buf, sizeof(grp_buf));

		RTE_LOG(INFO, M2U, "  Group %s: %u subscribers\n",
			grp_buf, subs->count);
		for (uint32_t i = 0; i < subs->count; i++) {
			a.s_addr = subs->entries[i].unicast_ip;
			inet_ntop(AF_INET, &a, sub_buf, sizeof(sub_buf));
			RTE_LOG(INFO, M2U, "    -> %s:%u (port %u)\n",
				sub_buf,
				rte_be_to_cpu_16(subs->entries[i].unicast_port),
				subs->entries[i].tx_port);
		}
	}
	RTE_LOG(INFO, M2U, "===================\n");
}
