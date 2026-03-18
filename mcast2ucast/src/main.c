#include <signal.h>
#include <string.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_ring.h>
#include <rte_malloc.h>

#include "config.h"
#include "group_table.h"
#include "rewrite.h"
#include "dataplane.h"
#include "control.h"
#include "producer_notify.h"
#include "stats.h"
#include "web.h"

#define RTE_LOGTYPE_M2U RTE_LOGTYPE_USER1

volatile int force_quit;

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		RTE_LOG(INFO, M2U, "\nSignal %d received, shutting down...\n",
			signum);
		force_quit = 1;
	}
}

static int
port_init(uint16_t port_id, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf;
	struct rte_eth_dev_info dev_info;
	int ret;

	memset(&port_conf, 0, sizeof(port_conf));

	ret = rte_eth_dev_info_get(port_id, &dev_info);
	if (ret != 0) {
		RTE_LOG(ERR, M2U, "Cannot get device info for port %u: %s\n",
			port_id, rte_strerror(-ret));
		return ret;
	}

	port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
	port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

	/*
	 * Do NOT enable MBUF_FAST_FREE — it assumes refcnt==1 and single pool,
	 * which is incompatible with our zero-copy clone/refcnt approach.
	 */

	ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
	if (ret != 0) {
		RTE_LOG(ERR, M2U, "Cannot configure port %u: %s\n",
			port_id, rte_strerror(-ret));
		return ret;
	}

	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
	if (ret != 0) {
		RTE_LOG(ERR, M2U,
			"Cannot adjust descriptors for port %u: %s\n",
			port_id, rte_strerror(-ret));
		return ret;
	}

	unsigned int socket_id = rte_eth_dev_socket_id(port_id);
	if (socket_id == (unsigned int)SOCKET_ID_ANY)
		socket_id = 0;

	ret = rte_eth_rx_queue_setup(port_id, 0, nb_rxd, socket_id,
				     NULL, mbuf_pool);
	if (ret < 0) {
		RTE_LOG(ERR, M2U, "RX queue setup failed for port %u: %s\n",
			port_id, rte_strerror(-ret));
		return ret;
	}

	ret = rte_eth_tx_queue_setup(port_id, 0, nb_txd, socket_id, NULL);
	if (ret < 0) {
		RTE_LOG(ERR, M2U, "TX queue setup failed for port %u: %s\n",
			port_id, rte_strerror(-ret));
		return ret;
	}

	ret = rte_eth_dev_start(port_id);
	if (ret < 0) {
		RTE_LOG(ERR, M2U, "Cannot start port %u: %s\n",
			port_id, rte_strerror(-ret));
		return ret;
	}

	/* Enable promiscuous mode to receive all multicast */
	ret = rte_eth_promiscuous_enable(port_id);
	if (ret != 0)
		RTE_LOG(WARNING, M2U,
			"Cannot enable promiscuous on port %u: %s\n",
			port_id, rte_strerror(-ret));

	struct rte_ether_addr mac;
	ret = rte_eth_macaddr_get(port_id, &mac);
	if (ret == 0) {
		RTE_LOG(INFO, M2U,
			"Port %u MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
			port_id,
			mac.addr_bytes[0], mac.addr_bytes[1],
			mac.addr_bytes[2], mac.addr_bytes[3],
			mac.addr_bytes[4], mac.addr_bytes[5]);
	}

	return 0;
}

static int
tap_init(struct app_config *cfg, struct rte_mempool *mbuf_pool)
{
	char vdev_args[128];
	int ret;

	snprintf(vdev_args, sizeof(vdev_args), "iface=%s", cfg->tap_name);
	ret = rte_eal_hotplug_add("vdev", "net_tap0", vdev_args);
	if (ret != 0) {
		RTE_LOG(ERR, M2U, "Cannot create TAP device '%s': %s\n",
			cfg->tap_name, rte_strerror(-ret));
		return ret;
	}

	ret = rte_eth_dev_get_port_by_name("net_tap0", &cfg->tap_port_id);
	if (ret != 0) {
		RTE_LOG(ERR, M2U, "Cannot find TAP port: %s\n",
			rte_strerror(-ret));
		return ret;
	}

	ret = port_init(cfg->tap_port_id, mbuf_pool);
	if (ret != 0)
		return ret;

	RTE_LOG(INFO, M2U, "TAP interface '%s' created (port %u)\n",
		cfg->tap_name, cfg->tap_port_id);
	return 0;
}

/*
 * Auto-detect local IP from the first non-loopback IPv4 interface.
 * Only used if --local-ip was not explicitly set.
 */
static void
detect_local_ip(struct app_config *cfg)
{
	struct ifaddrs *ifa_list, *ifa;

	if (cfg->local_ip != 0)
		return; /* already set via --local-ip */

	if (getifaddrs(&ifa_list) < 0)
		return;

	for (ifa = ifa_list; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;
		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;

		struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
		cfg->local_ip = sin->sin_addr.s_addr;

		char buf[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
		RTE_LOG(INFO, M2U,
			"Auto-detected local IP: %s (from %s)\n",
			buf, ifa->ifa_name);
		break;
	}

	freeifaddrs(ifa_list);
}

static int
load_static_subscribers(struct app_config *cfg)
{
	int loaded = 0;

	for (int i = 0; i < cfg->nb_static_subs; i++) {
		struct static_sub_entry *se = &cfg->static_subs[i];
		struct subscriber sub;

		memset(&sub, 0, sizeof(sub));
		sub.unicast_ip = se->unicast_ip;
		sub.unicast_port = rte_cpu_to_be_16(se->unicast_port);
		sub.tx_port = cfg->tx_port_id;
		sub.is_static = 1;
		sub.is_local = 0;
		sub.last_seen = 0; /* static entries never expire */

		if (se->has_mac)
			rte_ether_addr_copy(&se->dst_mac, &sub.dst_mac);
		/* else MAC stays zero — will need ARP or manual config */

		if (group_table_add_subscriber(se->group_ip, &sub) == 0)
			loaded++;
	}

	RTE_LOG(INFO, M2U, "Loaded %d static subscribers\n", loaded);
	return 0;
}

int
main(int argc, char **argv)
{
	struct app_config cfg;
	struct rte_ring *ctrl_ring;
	struct dataplane_args dp_args;
	struct control_args ctrl_args;
	int ret;
	unsigned int lcore_id;
	unsigned int dp_lcore = RTE_MAX_LCORE;
	unsigned int ctrl_lcore = RTE_MAX_LCORE;

	/* EAL init — consumes EAL arguments */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "EAL init failed\n");
	argc -= ret;
	argv += ret;

	force_quit = 0;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Parse application arguments */
	ret = config_parse_args(argc, argv, &cfg);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid arguments\n");

	/* Load config file */
	ret = config_load_file(&cfg);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot load config file\n");

	/* Auto-detect local IP if not set via --local-ip */
	detect_local_ip(&cfg);

	/* Check available ports */
	uint16_t nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No available Ethernet ports\n");

	RTE_LOG(INFO, M2U, "Available ports: %u\n", nb_ports);

	if (cfg.rx_port_id >= nb_ports || cfg.tx_port_id >= nb_ports)
		rte_exit(EXIT_FAILURE,
			 "Port ID out of range (have %u ports)\n", nb_ports);

	/* Initialize memory pools */
	ret = rewrite_init(rte_socket_id());
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot init rewrite engine\n");

	struct rte_mempool *mbuf_pool = rewrite_get_packet_pool();

	/* Initialize ports */
	ret = port_init(cfg.rx_port_id, mbuf_pool);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot init RX port %u\n",
			 cfg.rx_port_id);

	if (cfg.tx_port_id != cfg.rx_port_id) {
		ret = port_init(cfg.tx_port_id, mbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot init TX port %u\n",
				 cfg.tx_port_id);
	}

	/* TAP interface */
	if (cfg.use_tap) {
		ret = tap_init(&cfg, mbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot init TAP interface\n");
	}

	/* Initialize group table */
	ret = group_table_init();
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot init group table\n");

	/* Load static subscribers from config */
	load_static_subscribers(&cfg);

	/* Initialize producer notification (kernel UDP socket) */
	ret = producer_notify_init();
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot init producer notify\n");

	/* Create control ring (data plane -> control plane) */
	ctrl_ring = rte_ring_create("ctrl_ring", CTRL_RING_SIZE,
				    rte_socket_id(),
				    RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (ctrl_ring == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create control ring\n");

	group_table_dump();

	/* Initialize stats and start web dashboard */
	stats_init();
	if (web_start(8080) == 0)
		RTE_LOG(INFO, M2U, "Web dashboard at http://0.0.0.0:8080/\n");
	else
		RTE_LOG(WARNING, M2U, "Failed to start web dashboard\n");

	/* Assign lcores: need at least 2 (one for dp, one for ctrl) */
	unsigned int main_lcore = rte_get_main_lcore();

	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (dp_lcore == RTE_MAX_LCORE)
			dp_lcore = lcore_id;
		else if (ctrl_lcore == RTE_MAX_LCORE)
			ctrl_lcore = lcore_id;
	}

	/*
	 * If only 1 worker lcore, run control plane on main lcore.
	 * If no worker lcores, run everything on main lcore.
	 */
	if (dp_lcore == RTE_MAX_LCORE) {
		/* No worker lcores — run data plane on main, skip ctrl */
		RTE_LOG(WARNING, M2U,
			"Only main lcore available, control plane disabled. "
			"Use static subscribers via config file.\n");
		dp_lcore = main_lcore;
	}
	if (ctrl_lcore == RTE_MAX_LCORE && dp_lcore != main_lcore)
		ctrl_lcore = main_lcore;

	/* Prepare args */
	memset(&dp_args, 0, sizeof(dp_args));
	dp_args.cfg = &cfg;
	dp_args.ctrl_ring = ctrl_ring;

	memset(&ctrl_args, 0, sizeof(ctrl_args));
	ctrl_args.cfg = &cfg;
	ctrl_args.ctrl_ring = ctrl_ring;

	/* Launch worker lcores */
	if (dp_lcore != main_lcore) {
		ret = rte_eal_remote_launch(dataplane_loop, &dp_args,
					    dp_lcore);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot launch data plane on lcore %u\n",
				 dp_lcore);
	}

	if (ctrl_lcore != RTE_MAX_LCORE && ctrl_lcore != main_lcore) {
		ret = rte_eal_remote_launch(control_loop, &ctrl_args,
					    ctrl_lcore);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot launch control plane on lcore %u\n",
				 ctrl_lcore);
	}

	/* Main lcore: run whichever plane is assigned here, or data plane */
	if (dp_lcore == main_lcore)
		dataplane_loop(&dp_args);
	else if (ctrl_lcore == main_lcore)
		control_loop(&ctrl_args);
	else {
		/* All planes on workers — main just waits */
		RTE_LOG(INFO, M2U, "Main lcore idle, waiting for signal...\n");
		while (!force_quit)
			rte_delay_us_sleep(100000);
	}

	/* Wait for workers */
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		rte_eal_wait_lcore(lcore_id);
	}

	/* Cleanup */
	RTE_LOG(INFO, M2U, "Cleaning up...\n");
	web_stop();

	uint16_t port_id;
	RTE_ETH_FOREACH_DEV(port_id) {
		RTE_LOG(INFO, M2U, "Stopping port %u\n", port_id);
		rte_eth_dev_stop(port_id);
		rte_eth_dev_close(port_id);
	}

	producer_notify_cleanup();
	group_table_destroy();
	rewrite_cleanup();
	rte_ring_free(ctrl_ring);
	rte_eal_cleanup();

	RTE_LOG(INFO, M2U, "Bye.\n");
	return 0;
}
