#ifndef MCAST2UCAST_STATS_H
#define MCAST2UCAST_STATS_H

#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

struct dp_stats {
	_Atomic uint64_t rx_packets;
	_Atomic uint64_t tx_packets;
	_Atomic uint64_t mcast_packets;
	_Atomic uint64_t drop_packets;
	_Atomic uint64_t igmp_packets;
	time_t           start_time;
};

/* Single global instance */
extern struct dp_stats g_stats;

static inline void stats_init(void)
{
	atomic_store(&g_stats.rx_packets, 0);
	atomic_store(&g_stats.tx_packets, 0);
	atomic_store(&g_stats.mcast_packets, 0);
	atomic_store(&g_stats.drop_packets, 0);
	atomic_store(&g_stats.igmp_packets, 0);
	g_stats.start_time = time(NULL);
}

#endif
