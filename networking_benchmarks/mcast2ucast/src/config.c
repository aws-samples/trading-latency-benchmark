#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <rte_log.h>

#define RTE_LOGTYPE_M2U RTE_LOGTYPE_USER1

static const struct option long_options[] = {
	{"rx-port",   required_argument, NULL, 'r'},
	{"tx-port",   required_argument, NULL, 'x'},
	{"config",    required_argument, NULL, 'c'},
	{"tap",       required_argument, NULL, 't'},
	{"local-ip",  required_argument, NULL, 'l'},
	{"ctrl-port", required_argument, NULL, 'p'},
	{NULL, 0, NULL, 0}
};

static void
config_set_defaults(struct app_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->rx_port_id = 0;
	cfg->tx_port_id = 0;
	cfg->use_tap = 0;
	strncpy(cfg->tap_name, "mcast0", sizeof(cfg->tap_name) - 1);
	cfg->ctrl_port = 9000;
	cfg->local_ip = 0;
}

int
config_parse_args(int argc, char **argv, struct app_config *cfg)
{
	int opt, opt_idx;
	struct in_addr addr;

	config_set_defaults(cfg);

	while ((opt = getopt_long(argc, argv, "r:x:c:t:l:p:",
				  long_options, &opt_idx)) != -1) {
		switch (opt) {
		case 'r':
			cfg->rx_port_id = (uint16_t)atoi(optarg);
			break;
		case 'x':
			cfg->tx_port_id = (uint16_t)atoi(optarg);
			break;
		case 'c':
			strncpy(cfg->config_file, optarg,
				sizeof(cfg->config_file) - 1);
			break;
		case 't':
			cfg->use_tap = 1;
			strncpy(cfg->tap_name, optarg,
				sizeof(cfg->tap_name) - 1);
			break;
		case 'l':
			if (inet_pton(AF_INET, optarg, &addr) != 1) {
				RTE_LOG(ERR, M2U, "Invalid local IP: %s\n",
					optarg);
				return -1;
			}
			cfg->local_ip = addr.s_addr;
			break;
		case 'p':
			cfg->ctrl_port = (uint16_t)atoi(optarg);
			break;
		default:
			RTE_LOG(ERR, M2U,
				"Usage: mcast2ucast [EAL opts] -- "
				"--rx-port <id> --tx-port <id> "
				"--config <file> [--tap <name>] "
				"[--local-ip <ip>] [--ctrl-port <port>]\n");
			return -1;
		}
	}

	/* Reset getopt for potential re-use */
	optind = 1;
	return 0;
}

static int
parse_mac(const char *str, struct rte_ether_addr *mac)
{
	unsigned int bytes[6];
	if (sscanf(str, "%x:%x:%x:%x:%x:%x",
		   &bytes[0], &bytes[1], &bytes[2],
		   &bytes[3], &bytes[4], &bytes[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		mac->addr_bytes[i] = (uint8_t)bytes[i];
	return 0;
}

int
config_load_file(struct app_config *cfg)
{
	FILE *fp;
	char line[512];
	struct in_addr addr;

	if (cfg->config_file[0] == '\0')
		return 0; /* no config file */

	fp = fopen(cfg->config_file, "r");
	if (fp == NULL) {
		RTE_LOG(ERR, M2U, "Cannot open config file: %s\n",
			cfg->config_file);
		return -1;
	}

	while (fgets(line, sizeof(line), fp) != NULL) {
		char *p = line;

		/* skip leading whitespace */
		while (*p == ' ' || *p == '\t')
			p++;
		/* skip comments and blank lines */
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;

		if (strncmp(p, "producer", 8) == 0) {
			char grp[64], pip[64];
			int pport;
			if (sscanf(p, "producer %63s %63s %d",
				   grp, pip, &pport) != 3) {
				RTE_LOG(WARNING, M2U,
					"Bad producer line: %s", line);
				continue;
			}
			if (cfg->nb_producers >= MAX_PRODUCERS) {
				RTE_LOG(WARNING, M2U,
					"Too many producers, skipping\n");
				continue;
			}
			struct producer_entry *e =
				&cfg->producers[cfg->nb_producers];
			if (inet_pton(AF_INET, grp, &addr) != 1) {
				RTE_LOG(WARNING, M2U,
					"Bad group IP: %s\n", grp);
				continue;
			}
			e->group_ip = addr.s_addr;
			if (inet_pton(AF_INET, pip, &addr) != 1) {
				RTE_LOG(WARNING, M2U,
					"Bad producer IP: %s\n", pip);
				continue;
			}
			e->producer_ip = addr.s_addr;
			e->producer_port = (uint16_t)pport;
			cfg->nb_producers++;

		} else if (strncmp(p, "subscriber", 10) == 0) {
			char grp[64], uip[64], mac_str[32];
			int uport;
			int nfields = sscanf(p,
				"subscriber %63s %63s %d %31s",
				grp, uip, &uport, mac_str);
			if (nfields < 3) {
				RTE_LOG(WARNING, M2U,
					"Bad subscriber line: %s", line);
				continue;
			}
			if (cfg->nb_static_subs >= MAX_STATIC_SUBS) {
				RTE_LOG(WARNING, M2U,
					"Too many static subs, skipping\n");
				continue;
			}
			struct static_sub_entry *e =
				&cfg->static_subs[cfg->nb_static_subs];
			if (inet_pton(AF_INET, grp, &addr) != 1) {
				RTE_LOG(WARNING, M2U,
					"Bad group IP: %s\n", grp);
				continue;
			}
			e->group_ip = addr.s_addr;
			if (inet_pton(AF_INET, uip, &addr) != 1) {
				RTE_LOG(WARNING, M2U,
					"Bad unicast IP: %s\n", uip);
				continue;
			}
			e->unicast_ip = addr.s_addr;
			e->unicast_port = (uint16_t)uport;
			memset(&e->dst_mac, 0, sizeof(e->dst_mac));
			e->has_mac = 0;
			if (nfields >= 4 &&
			    parse_mac(mac_str, &e->dst_mac) == 0)
				e->has_mac = 1;
			cfg->nb_static_subs++;

		} else {
			RTE_LOG(WARNING, M2U,
				"Unknown config directive: %s", line);
		}
	}

	fclose(fp);

	RTE_LOG(INFO, M2U,
		"Config: %d producers, %d static subscribers\n",
		cfg->nb_producers, cfg->nb_static_subs);
	return 0;
}

const struct producer_entry *
config_find_producer(const struct app_config *cfg, uint32_t group_ip)
{
	for (int i = 0; i < cfg->nb_producers; i++) {
		if (cfg->producers[i].group_ip == group_ip)
			return &cfg->producers[i];
	}
	return NULL;
}
