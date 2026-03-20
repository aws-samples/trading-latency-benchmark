#ifndef MCAST2UCAST_PRODUCER_NOTIFY_H
#define MCAST2UCAST_PRODUCER_NOTIFY_H

#include <stdint.h>
#include "config.h"

/* TLV message types for producer control protocol */
#define NOTIFY_TYPE_SUBSCRIBE   0x01
#define NOTIFY_TYPE_UNSUBSCRIBE 0x02
#define NOTIFY_TYPE_KEEPALIVE   0x03

/* Wire format: 16 bytes total */
struct __attribute__((packed)) notify_msg {
	uint8_t  type;
	uint8_t  reserved;
	uint16_t subscriber_port; /* network byte order */
	uint32_t group_ip;        /* network byte order */
	uint32_t subscriber_ip;   /* network byte order */
	uint32_t source_ip;       /* network byte order, 0 = any (ASM) */
};

int  producer_notify_init(void);
void producer_notify_cleanup(void);

int producer_notify_subscribe(const struct app_config *cfg,
			      uint32_t group_ip,
			      uint32_t subscriber_ip,
			      uint16_t subscriber_port);

int producer_notify_unsubscribe(const struct app_config *cfg,
				uint32_t group_ip,
				uint32_t subscriber_ip,
				uint16_t subscriber_port);

int producer_notify_keepalive(const struct app_config *cfg,
			      uint32_t group_ip,
			      uint32_t subscriber_ip,
			      uint16_t subscriber_port);

#endif
