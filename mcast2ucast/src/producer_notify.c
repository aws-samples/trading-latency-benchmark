#include "producer_notify.h"

#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <rte_log.h>

#define RTE_LOGTYPE_M2U RTE_LOGTYPE_USER1

static int notify_sock = -1;

int
producer_notify_init(void)
{
	notify_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (notify_sock < 0) {
		RTE_LOG(ERR, M2U, "Cannot create notification socket\n");
		return -1;
	}
	return 0;
}

void
producer_notify_cleanup(void)
{
	if (notify_sock >= 0) {
		close(notify_sock);
		notify_sock = -1;
	}
}

static int
send_notify(const struct app_config *cfg, uint32_t group_ip,
	    uint32_t subscriber_ip, uint16_t subscriber_port,
	    uint8_t type)
{
	const struct producer_entry *prod;
	struct notify_msg msg;
	struct sockaddr_in dst;

	prod = config_find_producer(cfg, group_ip);
	if (prod == NULL) {
		char buf[INET_ADDRSTRLEN];
		struct in_addr a = { .s_addr = group_ip };
		inet_ntop(AF_INET, &a, buf, sizeof(buf));
		RTE_LOG(DEBUG, M2U,
			"No producer configured for group %s\n", buf);
		return -1;
	}

	memset(&msg, 0, sizeof(msg));
	msg.type = type;
	msg.group_ip = group_ip;
	msg.subscriber_ip = subscriber_ip;
	msg.subscriber_port = rte_cpu_to_be_16(subscriber_port);
	msg.source_ip = 0; /* ASM */

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_addr.s_addr = prod->producer_ip;
	dst.sin_port = htons(prod->producer_port);

	ssize_t n = sendto(notify_sock, &msg, sizeof(msg), 0,
			   (struct sockaddr *)&dst, sizeof(dst));
	if (n < 0) {
		RTE_LOG(ERR, M2U,
			"Failed to send notification to producer\n");
		return -1;
	}

	{
		char gbuf[INET_ADDRSTRLEN], pbuf[INET_ADDRSTRLEN];
		struct in_addr a;
		a.s_addr = group_ip;
		inet_ntop(AF_INET, &a, gbuf, sizeof(gbuf));
		a.s_addr = prod->producer_ip;
		inet_ntop(AF_INET, &a, pbuf, sizeof(pbuf));
		const char *type_str =
			type == NOTIFY_TYPE_SUBSCRIBE   ? "SUBSCRIBE" :
			type == NOTIFY_TYPE_UNSUBSCRIBE ? "UNSUBSCRIBE" :
			type == NOTIFY_TYPE_KEEPALIVE   ? "KEEPALIVE" :
			"UNKNOWN";
		if (type == NOTIFY_TYPE_KEEPALIVE)
			RTE_LOG(DEBUG, M2U,
				"Sent %s for group %s to producer %s:%u\n",
				type_str, gbuf, pbuf, prod->producer_port);
		else
			RTE_LOG(INFO, M2U,
				"Sent %s for group %s to producer %s:%u\n",
				type_str, gbuf, pbuf, prod->producer_port);
	}

	return 0;
}

int
producer_notify_subscribe(const struct app_config *cfg,
			  uint32_t group_ip,
			  uint32_t subscriber_ip,
			  uint16_t subscriber_port)
{
	return send_notify(cfg, group_ip, subscriber_ip, subscriber_port,
			   NOTIFY_TYPE_SUBSCRIBE);
}

int
producer_notify_unsubscribe(const struct app_config *cfg,
			    uint32_t group_ip,
			    uint32_t subscriber_ip,
			    uint16_t subscriber_port)
{
	return send_notify(cfg, group_ip, subscriber_ip, subscriber_port,
			   NOTIFY_TYPE_UNSUBSCRIBE);
}

int
producer_notify_keepalive(const struct app_config *cfg,
			  uint32_t group_ip,
			  uint32_t subscriber_ip,
			  uint16_t subscriber_port)
{
	return send_notify(cfg, group_ip, subscriber_ip, subscriber_port,
			   NOTIFY_TYPE_KEEPALIVE);
}
