#ifndef MCAST2UCAST_CONTROL_H
#define MCAST2UCAST_CONTROL_H

#include <rte_ring.h>
#include "config.h"

struct control_args {
	struct app_config *cfg;
	struct rte_ring   *ctrl_ring;  /* receives IGMP packets from data plane */
};

/* Main control-plane loop — runs on an lcore */
int control_loop(void *arg);

#endif
