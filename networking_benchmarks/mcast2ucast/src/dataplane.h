#ifndef MCAST2UCAST_DATAPLANE_H
#define MCAST2UCAST_DATAPLANE_H

#include <rte_ring.h>
#include "config.h"

struct dataplane_args {
	struct app_config *cfg;
	struct rte_ring   *ctrl_ring;  /* punt IGMP here for control plane */
};

/* Main data-plane loop — runs on an lcore, never returns until force_quit */
int dataplane_loop(void *arg);

#endif
