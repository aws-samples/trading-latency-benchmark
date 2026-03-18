#include "control.h"
#include "group_table.h"
#include "producer_notify.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_log.h>
#include <rte_cycles.h>

#define RTE_LOGTYPE_M2U RTE_LOGTYPE_USER1

extern volatile int force_quit;

/* Keepalive interval and subscriber expiry */
#define KEEPALIVE_INTERVAL_SEC 30
#define SUBSCRIBER_EXPIRY_SEC  90

/* IGMP message types */
#define IGMP_MEMBERSHIP_QUERY     0x11
#define IGMP_V1_MEMBERSHIP_REPORT 0x12
#define IGMP_V2_MEMBERSHIP_REPORT 0x16
#define IGMP_V2_LEAVE_GROUP       0x17
#define IGMP_V3_MEMBERSHIP_REPORT 0x22

struct igmp_hdr {
	uint8_t  type;
	uint8_t  max_resp_time;
	uint16_t checksum;
	uint32_t group_addr;  /* network byte order */
};

/* IGMPv3 report structures */
struct igmpv3_group_record {
	uint8_t  record_type;
	uint8_t  aux_data_len;
	uint16_t num_sources;
	uint32_t multicast_addr;  /* network byte order */
	/* followed by source addresses */
};

struct igmpv3_report {
	uint8_t  type;
	uint8_t  reserved1;
	uint16_t checksum;
	uint16_t reserved2;
	uint16_t num_group_records;
};

/* IGMPv3 group record types */
#define IGMPV3_MODE_IS_INCLUDE 1
#define IGMPV3_MODE_IS_EXCLUDE 2
#define IGMPV3_CHANGE_TO_INCLUDE 3
#define IGMPV3_CHANGE_TO_EXCLUDE 4
#define IGMPV3_ALLOW_NEW_SOURCES 5
#define IGMPV3_BLOCK_OLD_SOURCES 6

/* ------------------------------------------------------------------ */
/*  Local IGMP handling (subscriber side)                              */
/* ------------------------------------------------------------------ */

static void
handle_igmp_join(struct app_config *cfg, uint32_t group_ip, uint32_t src_ip)
{
	struct subscriber sub;
	char gbuf[INET_ADDRSTRLEN], sbuf[INET_ADDRSTRLEN];
	struct in_addr a;

	a.s_addr = group_ip;
	inet_ntop(AF_INET, &a, gbuf, sizeof(gbuf));
	a.s_addr = src_ip;
	inet_ntop(AF_INET, &a, sbuf, sizeof(sbuf));

	RTE_LOG(INFO, M2U, "IGMP join: group=%s from=%s\n", gbuf, sbuf);

	/* Add as a local subscriber wanting TAP delivery */
	memset(&sub, 0, sizeof(sub));
	sub.unicast_ip = src_ip;
	sub.unicast_port = 0; /* local delivery via TAP */
	sub.tx_port = cfg->tx_port_id;
	sub.is_static = 0;
	sub.is_local = 1;
	sub.last_seen = time(NULL);

	group_table_add_subscriber(group_ip, &sub);

	/* Notify the multicast producer via unicast UDP */
	if (cfg->local_ip != 0)
		producer_notify_subscribe(cfg, group_ip,
					  cfg->local_ip, cfg->ctrl_port);
}

static void
handle_igmp_leave(struct app_config *cfg, uint32_t group_ip, uint32_t src_ip)
{
	char gbuf[INET_ADDRSTRLEN];
	struct in_addr a = { .s_addr = group_ip };
	inet_ntop(AF_INET, &a, gbuf, sizeof(gbuf));

	RTE_LOG(INFO, M2U, "IGMP leave: group=%s\n", gbuf);

	group_table_remove_subscriber(group_ip, src_ip, 0);

	if (cfg->local_ip != 0)
		producer_notify_unsubscribe(cfg, group_ip,
					    cfg->local_ip, cfg->ctrl_port);
}

static void
process_igmpv3_report(struct app_config *cfg, struct rte_mbuf *pkt,
		      uint32_t src_ip)
{
	struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)
		((char *)eth + sizeof(struct rte_ether_hdr));
	uint16_t ip_hdr_len = (uint16_t)((ip->version_ihl & 0x0f) * 4);

	struct igmpv3_report *report = (struct igmpv3_report *)
		((char *)ip + ip_hdr_len);

	uint16_t num_records = rte_be_to_cpu_16(report->num_group_records);
	struct igmpv3_group_record *rec = (struct igmpv3_group_record *)
		((char *)report + sizeof(struct igmpv3_report));

	for (uint16_t i = 0; i < num_records; i++) {
		uint32_t group = rec->multicast_addr;
		uint16_t nsrc = rte_be_to_cpu_16(rec->num_sources);

		switch (rec->record_type) {
		case IGMPV3_MODE_IS_EXCLUDE:
		case IGMPV3_CHANGE_TO_EXCLUDE:
			handle_igmp_join(cfg, group, src_ip);
			break;
		case IGMPV3_MODE_IS_INCLUDE:
		case IGMPV3_CHANGE_TO_INCLUDE:
			if (nsrc == 0)
				handle_igmp_leave(cfg, group, src_ip);
			else
				handle_igmp_join(cfg, group, src_ip);
			break;
		case IGMPV3_ALLOW_NEW_SOURCES:
			handle_igmp_join(cfg, group, src_ip);
			break;
		case IGMPV3_BLOCK_OLD_SOURCES:
			break;
		}

		rec = (struct igmpv3_group_record *)
			((char *)rec + sizeof(*rec) + nsrc * 4 +
			 rec->aux_data_len * 4);
	}
}

static void
process_igmp(struct app_config *cfg, struct rte_mbuf *pkt)
{
	struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)
		((char *)eth + sizeof(struct rte_ether_hdr));
	uint16_t ip_hdr_len = (uint16_t)((ip->version_ihl & 0x0f) * 4);
	uint32_t src_ip = ip->src_addr;

	struct igmp_hdr *igmp = (struct igmp_hdr *)
		((char *)ip + ip_hdr_len);

	switch (igmp->type) {
	case IGMP_V2_MEMBERSHIP_REPORT:
	case IGMP_V1_MEMBERSHIP_REPORT:
		handle_igmp_join(cfg, igmp->group_addr, src_ip);
		break;

	case IGMP_V2_LEAVE_GROUP:
		handle_igmp_leave(cfg, igmp->group_addr, src_ip);
		break;

	case IGMP_V3_MEMBERSHIP_REPORT:
		process_igmpv3_report(cfg, pkt, src_ip);
		break;

	case IGMP_MEMBERSHIP_QUERY:
		RTE_LOG(DEBUG, M2U, "Received IGMP query (ignored)\n");
		break;

	default:
		RTE_LOG(DEBUG, M2U, "Unknown IGMP type: 0x%02x\n",
			igmp->type);
		break;
	}
}

/* ------------------------------------------------------------------ */
/*  Control port listener (producer side)                              */
/*  Receives SUBSCRIBE/UNSUBSCRIBE/KEEPALIVE from remote subscribers  */
/* ------------------------------------------------------------------ */

static int
ctrl_listener_init(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	/* Non-blocking so the control loop can poll */
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		RTE_LOG(ERR, M2U, "Cannot bind control port %u: %s\n",
			port, strerror(errno));
		close(fd);
		return -1;
	}

	RTE_LOG(INFO, M2U,
		"Control port listener on UDP %u (for remote SUBSCRIBE/UNSUBSCRIBE)\n",
		port);
	return fd;
}

static void
handle_ctrl_message(struct app_config *cfg, const struct notify_msg *msg,
		    const struct sockaddr_in *from)
{
	char gbuf[INET_ADDRSTRLEN], sbuf[INET_ADDRSTRLEN], fbuf[INET_ADDRSTRLEN];
	struct in_addr a;

	a.s_addr = msg->group_ip;
	inet_ntop(AF_INET, &a, gbuf, sizeof(gbuf));
	a.s_addr = msg->subscriber_ip;
	inet_ntop(AF_INET, &a, sbuf, sizeof(sbuf));
	inet_ntop(AF_INET, &from->sin_addr, fbuf, sizeof(fbuf));

	uint16_t sub_port = rte_be_to_cpu_16(msg->subscriber_port);

	switch (msg->type) {
	case NOTIFY_TYPE_SUBSCRIBE: {
		RTE_LOG(INFO, M2U,
			"Remote SUBSCRIBE: group=%s subscriber=%s:%u from=%s\n",
			gbuf, sbuf, sub_port, fbuf);

		struct subscriber sub;
		memset(&sub, 0, sizeof(sub));
		sub.unicast_ip = msg->subscriber_ip;
		sub.unicast_port = msg->subscriber_port;
		sub.tx_port = cfg->tx_port_id;
		sub.is_static = 0;
		sub.is_local = 0;
		sub.last_seen = time(NULL);
		/* MAC will be zero — needs ARP or manual config.
		 * For ENA TX in VPC, the VPC resolves MACs automatically. */

		group_table_add_subscriber(msg->group_ip, &sub);
		break;
	}

	case NOTIFY_TYPE_UNSUBSCRIBE:
		RTE_LOG(INFO, M2U,
			"Remote UNSUBSCRIBE: group=%s subscriber=%s:%u from=%s\n",
			gbuf, sbuf, sub_port, fbuf);

		group_table_remove_subscriber(msg->group_ip,
					      msg->subscriber_ip,
					      msg->subscriber_port);
		break;

	case NOTIFY_TYPE_KEEPALIVE:
		RTE_LOG(DEBUG, M2U,
			"KEEPALIVE: group=%s subscriber=%s:%u\n",
			gbuf, sbuf, sub_port);

		if (group_table_refresh_subscriber(msg->group_ip,
						   msg->subscriber_ip,
						   msg->subscriber_port) < 0) {
			/* Subscriber not found — treat as subscribe */
			RTE_LOG(INFO, M2U,
				"Keepalive for unknown subscriber %s:%u, "
				"auto-subscribing to %s\n",
				sbuf, sub_port, gbuf);

			struct subscriber sub;
			memset(&sub, 0, sizeof(sub));
			sub.unicast_ip = msg->subscriber_ip;
			sub.unicast_port = msg->subscriber_port;
			sub.tx_port = cfg->tx_port_id;
			sub.is_static = 0;
			sub.is_local = 0;
			sub.last_seen = time(NULL);
			group_table_add_subscriber(msg->group_ip, &sub);
		}
		break;

	default:
		RTE_LOG(WARNING, M2U,
			"Unknown control message type 0x%02x from %s\n",
			msg->type, fbuf);
		break;
	}
}

static void
ctrl_listener_poll(int fd, struct app_config *cfg)
{
	struct notify_msg msg;
	struct sockaddr_in from;
	socklen_t fromlen;

	for (;;) {
		fromlen = sizeof(from);
		ssize_t n = recvfrom(fd, &msg, sizeof(msg), 0,
				     (struct sockaddr *)&from, &fromlen);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			break;
		}
		if (n < (ssize_t)sizeof(msg)) {
			RTE_LOG(WARNING, M2U,
				"Short control message (%zd bytes)\n", n);
			continue;
		}
		handle_ctrl_message(cfg, &msg, &from);
	}
}

/* ------------------------------------------------------------------ */
/*  Keepalive sender (subscriber side)                                 */
/*  Periodically re-sends SUBSCRIBE to producers for active groups    */
/* ------------------------------------------------------------------ */

struct keepalive_ctx {
	struct app_config *cfg;
	int sent;
};

static int
keepalive_iter_cb(uint32_t group_ip,
		  const struct subscriber_list *subs __rte_unused,
		  void *ctx)
{
	struct keepalive_ctx *kctx = ctx;
	struct app_config *cfg = kctx->cfg;

	if (cfg->local_ip == 0)
		return 0;

	/* Only send keepalive if we have a producer for this group */
	if (config_find_producer(cfg, group_ip) != NULL) {
		producer_notify_keepalive(cfg, group_ip,
					 cfg->local_ip, cfg->ctrl_port);
		kctx->sent++;
	}
	return 0;
}

static void
send_keepalives(struct app_config *cfg)
{
	struct keepalive_ctx kctx = { .cfg = cfg, .sent = 0 };
	group_table_iterate(keepalive_iter_cb, &kctx);
	if (kctx.sent > 0)
		RTE_LOG(DEBUG, M2U, "Sent %d keepalives to producers\n",
			kctx.sent);
}

/* ------------------------------------------------------------------ */
/*  Main control loop                                                  */
/* ------------------------------------------------------------------ */

int
control_loop(void *arg)
{
	struct control_args *cargs = arg;
	struct app_config *cfg = cargs->cfg;
	struct rte_ring *ctrl_ring = cargs->ctrl_ring;
	struct rte_mbuf *pkts[32];
	unsigned int lcore_id = rte_lcore_id();

	/* Start the control port listener for remote SUBSCRIBE/UNSUBSCRIBE */
	int ctrl_fd = ctrl_listener_init(cfg->ctrl_port);
	if (ctrl_fd < 0)
		RTE_LOG(WARNING, M2U,
			"Control port listener disabled (bind failed)\n");

	time_t last_keepalive = time(NULL);
	time_t last_expiry = time(NULL);

	RTE_LOG(INFO, M2U, "Control plane running on lcore %u\n", lcore_id);

	while (!force_quit) {
		/* 1. Dequeue IGMP packets punted from data plane */
		unsigned int n = rte_ring_dequeue_burst(ctrl_ring,
			(void **)pkts, 32, NULL);

		for (unsigned int i = 0; i < n; i++) {
			process_igmp(cfg, pkts[i]);
			rte_pktmbuf_free(pkts[i]);
		}

		/* 2. Poll control port for remote SUBSCRIBE/UNSUBSCRIBE */
		if (ctrl_fd >= 0)
			ctrl_listener_poll(ctrl_fd, cfg);

		/* 3. Periodic keepalives to producers */
		time_t now = time(NULL);
		if (now - last_keepalive >= KEEPALIVE_INTERVAL_SEC) {
			send_keepalives(cfg);
			last_keepalive = now;
		}

		/* 4. Expire stale dynamic subscribers */
		if (now - last_expiry >= KEEPALIVE_INTERVAL_SEC) {
			int expired = group_table_expire_subscribers(
				SUBSCRIBER_EXPIRY_SEC);
			if (expired > 0)
				RTE_LOG(INFO, M2U,
					"Expired %d stale subscribers\n",
					expired);
			last_expiry = now;
		}

		/* Yield CPU briefly if no work */
		if (n == 0)
			rte_delay_us_sleep(100);
	}

	if (ctrl_fd >= 0)
		close(ctrl_fd);

	RTE_LOG(INFO, M2U, "Control plane lcore %u exiting\n", lcore_id);
	return 0;
}
