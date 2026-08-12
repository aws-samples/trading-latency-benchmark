/*
 * AF_XDP multicast receiver (light m2u tunnel).
 *
 * Attaches mcast.o to the NIC via XDP, opens an AF_XDP
 * socket on the chosen queue, and polls the RX ring directly — no
 * kernel IP stack involvement after the XDP redirect.
 *
 * Packet layout received (starting from Ethernet header):
 *   Eth | IPv4 | UDP | m2u{ magic(4), group(4) } | payload
 *
 * Requires root (CAP_NET_ADMIN for XDP attach, CAP_NET_RAW for AF_XDP).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <algorithm>
#include <vector>
#include <string>

#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <sys/resource.h>

#include <xdp/xsk.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <linux/if_link.h>

/* ── defaults ─────────────────────────────────────────────────────────── */
static constexpr int      DEF_PORT    = 5000;
static constexpr int      DEF_COUNT   = 10000;
static constexpr int      DEF_TIMEOUT = 60;
static constexpr int      DEF_QUEUE   = 0;
static constexpr uint32_t NUM_FRAMES  = 4096;
static constexpr uint32_t FRAME_SIZE  = 4096;   /* XSK_UMEM__DEFAULT_FRAME_SIZE */
static constexpr uint32_t FILL_SIZE   = 2048;   /* XSK_RING_PROD__DEFAULT_NUM_DESCS */
static constexpr uint32_t RX_SIZE     = 2048;
static constexpr uint32_t BATCH       = 64;
static constexpr uint16_t ETH_P_IPV4  = 0x0800;
static constexpr int      HDR_SIZE    = 32;     /* seq(8) + ts_ns(8) + replicator_ns(8) + replicator_tx_ns(8) */
static constexpr uint32_t M2U_MAGIC   = 0x4D324355;  /* "M2CU" — light mcast->ucast tag */
static constexpr int      M2U_HDR_LEN = 8;           /* magic(4) + group(4) */

struct __attribute__((packed)) pkt_hdr {
	uint64_t seq;
	uint64_t ts_ns;
	uint64_t replicator_ns;     /* 0 if no replicator stamp; non-zero enables per-hop breakdown */
	uint64_t replicator_tx_ns;  /* replicator stamp just before TX submit; splits hop2 */
};

/* ── globals for signal handler cleanup ──────────────────────────────── */
static volatile sig_atomic_t g_stop        = 0;
static struct bpf_object    *g_bpf_obj     = nullptr;
static int                   g_xdp_prog_fd = -1;
static uint32_t              g_xdp_flags   = 0;
static int                   g_ifindex     = 0;
static struct xsk_socket     *g_xsk        = nullptr;
static struct xsk_umem       *g_umem       = nullptr;
static void                  *g_umem_buf   = nullptr;

static void sig_handler(int) { g_stop = 1; }

static void cleanup()
{
	if (g_xsk)      { xsk_socket__delete(g_xsk);  g_xsk = nullptr; }
	if (g_umem)     { xsk_umem__delete(g_umem);   g_umem = nullptr; }
	if (g_umem_buf) { free(g_umem_buf);            g_umem_buf = nullptr; }
	if (g_xdp_prog_fd >= 0 && g_ifindex) {
		bpf_xdp_detach(g_ifindex, g_xdp_flags, nullptr);
		g_xdp_prog_fd = -1;
	}
	if (g_bpf_obj) { bpf_object__close(g_bpf_obj); g_bpf_obj = nullptr; }
}

/* ── timing ───────────────────────────────────────────────────────────── */
static inline uint64_t now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t betoh64_(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(v);
#else
	return v;
#endif
}

/* ── percentile helper ────────────────────────────────────────────────── */
static uint64_t pct(std::vector<uint64_t> &sorted, int p)
{
	if (sorted.empty()) return 0;
	size_t idx = (size_t)((sorted.size() - 1) * p) / 100;
	return sorted[idx];
}

/* ── usage ────────────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
	printf("Usage: %s [options]\n"
	       "  -I <iface>   network interface (required)\n"
	       "  -g <group>   inner multicast group to match (default: 224.0.31.50)\n"
	       "  -p <port>    inner UDP dst port to match (default: %d)\n"
	       "  -c <count>   packets to receive        (default: %d)\n"
	       "  -t <timeout> seconds before giving up  (default: %d)\n"
	       "  -q <queue>   XDP/AF_XDP queue index    (default: %d)\n"
	       "  -r           print raw latencies (ns)\n"
	       "  -h           this help\n"
	       "\nRequires root (XDP attach + AF_XDP).\n",
	       prog, DEF_PORT, DEF_COUNT, DEF_TIMEOUT, DEF_QUEUE);
}

int main(int argc, char *argv[])
{
	const char *iface    = nullptr;
	const char *group    = "224.0.31.50";
	int  port    = DEF_PORT;
	int  count   = DEF_COUNT;
	int  timeout = DEF_TIMEOUT;
	int  queue   = DEF_QUEUE;
	bool raw     = false;
	const char *json_path = nullptr;

	int opt;
	while ((opt = getopt(argc, argv, "I:g:p:c:t:q:rj:h")) != -1) {
		switch (opt) {
		case 'I': iface    = optarg;           break;
		case 'g': group    = optarg;           break;
		case 'p': port     = atoi(optarg);     break;
		case 'c': count    = atoi(optarg);     break;
		case 't': timeout  = atoi(optarg);     break;
		case 'q': queue    = atoi(optarg);     break;
		case 'r': raw      = true;             break;
		case 'j': json_path = optarg;          break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}
	if (!iface) { fprintf(stderr, "error: -I <iface> is required\n"); usage(argv[0]); return 1; }

	/* ── resolve BPF object path (search order) ────────────────────────── */
	static const char *bpf_search_paths[] = {
		"./src/xdp/mcast.o",           // dev: running from af_xdp/ source tree
		"./xdp/mcast.o",               // installed: running from /opt/af-xdp/
		"/opt/af-xdp/xdp/mcast.o",    // absolute: baked AMI
		nullptr
	};
	const char *bpf_path = nullptr;
	for (const char **p = bpf_search_paths; *p; ++p) {
		if (access(*p, R_OK) == 0) { bpf_path = *p; break; }
	}
	if (!bpf_path) {
		fprintf(stderr, "error: mcast.o not found in search paths\n");
		return 1;
	}

	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);
	atexit(cleanup);

	/* ── resolve interface index ──────────────────────────────────────── */
	g_ifindex = (int)if_nametoindex(iface);
	if (!g_ifindex) { perror("if_nametoindex"); return 1; }

	/* ── load and attach XDP program (libbpf direct, no libxdp dispatcher) */
	g_bpf_obj = bpf_object__open_file(bpf_path, nullptr);
	if (!g_bpf_obj || libbpf_get_error(g_bpf_obj)) {
		fprintf(stderr, "error: bpf_object__open_file(%s): %s\n",
		        bpf_path, strerror(errno));
		return 1;
	}

	int err = bpf_object__load(g_bpf_obj);
	if (err) {
		fprintf(stderr, "error: bpf_object__load: %s\n", strerror(-err));
		return 1;
	}

	struct bpf_program *bpf_prog =
	    bpf_object__find_program_by_name(g_bpf_obj, "mcast");
	if (!bpf_prog) {
		fprintf(stderr, "error: XDP program 'mcast' not found in %s\n", bpf_path);
		return 1;
	}
	g_xdp_prog_fd = bpf_program__fd(bpf_prog);

	/* try native (driver) mode first, fall back to SKB */
	g_xdp_flags = XDP_FLAGS_DRV_MODE;
	err = bpf_xdp_attach(g_ifindex, g_xdp_prog_fd, g_xdp_flags, nullptr);
	if (err) {
		fprintf(stderr, "native XDP failed (%s), trying SKB mode\n", strerror(-err));
		g_xdp_flags = XDP_FLAGS_SKB_MODE;
		err = bpf_xdp_attach(g_ifindex, g_xdp_prog_fd, g_xdp_flags, nullptr);
	}
	if (err) {
		fprintf(stderr, "error: XDP attach failed: %s\n", strerror(-err));
		return 1;
	}

	/* ── get xsks_map fd from the loaded BPF object ───────────────────── */
	int map_fd = bpf_object__find_map_fd_by_name(g_bpf_obj, "xsks_map");
	if (map_fd < 0) {
		fprintf(stderr, "error: xsks_map not found in BPF object\n");
		return 1;
	}

	/* ── allocate UMEM ────────────────────────────────────────────────── */
	{
		/* Parity with mcast_send: raise memlock so UMEM registration is never
		 * charged against a small default RLIMIT_MEMLOCK on older kernels. */
		struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
		if (setrlimit(RLIMIT_MEMLOCK, &rl) < 0)
			perror("setrlimit RLIMIT_MEMLOCK (continuing)");
	}
	if (posix_memalign(&g_umem_buf, getpagesize(),
	                   (size_t)NUM_FRAMES * FRAME_SIZE) != 0) {
		perror("posix_memalign");
		return 1;
	}
	memset(g_umem_buf, 0, (size_t)NUM_FRAMES * FRAME_SIZE);

	struct xsk_ring_prod fq;
	struct xsk_ring_cons cq;
	const struct xsk_umem_config umem_cfg = {
		.fill_size      = FILL_SIZE,
		.comp_size      = FILL_SIZE,
		.frame_size     = FRAME_SIZE,
		.frame_headroom = 0,
		.flags          = 0,
	};
	err = xsk_umem__create(&g_umem, g_umem_buf,
	                       (uint64_t)NUM_FRAMES * FRAME_SIZE,
	                       &fq, &cq, &umem_cfg);
	if (err) { fprintf(stderr, "xsk_umem__create: %s\n", strerror(-err)); return 1; }

	/* ── create AF_XDP socket (RX only) ───────────────────────────────── */
	struct xsk_ring_cons rx;
	const struct xsk_socket_config xsk_cfg = {
		.rx_size       = RX_SIZE,
		.tx_size       = 0,
		.libbpf_flags  = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
		.xdp_flags     = 0,
		.bind_flags    = XDP_USE_NEED_WAKEUP,
	};
	err = xsk_socket__create(&g_xsk, iface, (uint32_t)queue,
	                         g_umem, &rx, nullptr, &xsk_cfg);
	if (err) { fprintf(stderr, "xsk_socket__create: %s\n", strerror(-err)); return 1; }

	/* ── register socket in xsks_map ─────────────────────────────────── */
	int xsk_fd = xsk_socket__fd(g_xsk);

	/* ── NAPI busy-poll ───────────────────────────────────────────────
	 * Drive RX in-app so drain latency does not depend on the NIC's
	 * gro_flush_timeout. With these set, the poll() in the RX loop busy-polls
	 * the NAPI for up to busy_us and pulls frames the instant they land,
	 * instead of waiting for the deferred-NAPI timer. Best paired with a small
	 * gro_flush_timeout (~20µs) as a safety net. Guards mirror XdpSocket. */
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif
	{
		int on = 1, busy_us = 50, budget = 64;
		setsockopt(xsk_fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &on, sizeof(on));
		setsockopt(xsk_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
		setsockopt(xsk_fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget));
	}

	if (bpf_map_update_elem(map_fd, &queue, &xsk_fd, BPF_ANY) < 0) {
		perror("bpf_map_update_elem(xsks_map)");
		return 1;
	}

	/* ── configure the mcast.o filter ─────────────────────────────────── */
	/* mcast.o only redirects m2u-tagged multicast packets whose {group,port}
	 * match an entry in config_map; otherwise it XDP_PASSes them to the kernel
	 * (and our AF_XDP socket never sees them). Seed slot 0 with our target. */
	{
		int cfg_fd = bpf_object__find_map_fd_by_name(g_bpf_obj, "config_map");
		if (cfg_fd < 0) {
			fprintf(stderr, "error: config_map not found in %s\n", bpf_path);
			return 1;
		}
		struct { uint32_t target_ip; uint16_t target_port; uint16_t padding; } mc_cfg;
		memset(&mc_cfg, 0, sizeof(mc_cfg));
		if (inet_pton(AF_INET, group, &mc_cfg.target_ip) != 1) {
			fprintf(stderr, "error: invalid multicast group '%s'\n", group);
			return 1;
		}
		mc_cfg.target_port = htons((uint16_t)port);
		uint32_t cfg_key = 0;
		if (bpf_map_update_elem(cfg_fd, &cfg_key, &mc_cfg, BPF_ANY) < 0) {
			perror("bpf_map_update_elem(config_map)");
			return 1;
		}
		printf("config_map[0] = %s:%d (mcast.o will redirect matches)\n", group, port);
	}

	/* ── pre-populate fill ring ───────────────────────────────────────── */
	/* Reserve as much of the fill ring as the driver actually grants. Some
	 * libbpf/driver combos size the ring below the requested fill_size, so a
	 * single all-or-nothing reserve of FILL_SIZE can return 0 ("fill ring
	 * reserve failed"). Back off by halves and use whatever we get. */
	uint32_t fill_idx = 0;
	uint32_t fill_want = FILL_SIZE;
	uint32_t fill_got  = 0;
	while (fill_want >= 64) {
		fill_got = xsk_ring_prod__reserve(&fq, fill_want, &fill_idx);
		if (fill_got > 0) break;
		fill_want /= 2;
	}
	if (fill_got == 0) {
		fprintf(stderr, "error: fill ring reserve failed (requested up to %u)\n", FILL_SIZE);
		return 1;
	}
	for (uint32_t i = 0; i < fill_got; i++)
		*xsk_ring_prod__fill_addr(&fq, fill_idx + i) = (uint64_t)i * FRAME_SIZE;
	xsk_ring_prod__submit(&fq, fill_got);

	/* ── stats ────────────────────────────────────────────────────────── */
	std::vector<uint64_t> latencies;       /* total: rx_ns - tx_ns */
	std::vector<uint64_t> latencies_hop1;  /* hop1:  replicator_ns - tx_ns   (source → replicator) */
	std::vector<uint64_t> latencies_hop2;  /* hop2:  rx_ns - replicator_ns   (replicator → destination) */
	/* hop2 sub-split (present when replicator_tx_ns is stamped): */
	std::vector<uint64_t> latencies_proc;  /*   replicator_tx_ns - replicator_ns  (relay processing) */
	std::vector<uint64_t> latencies_leg2;  /*   rx_ns - replicator_tx_ns          (wire + dest RX)   */
	latencies.reserve(count);
	latencies_hop1.reserve(count);
	latencies_hop2.reserve(count);
	latencies_proc.reserve(count);
	latencies_leg2.reserve(count);
	int      received      = 0;
	int      lost          = 0;
	int      ooo           = 0;
	int64_t  last_seq      = -1;
	uint64_t min_lat       = UINT64_MAX;
	uint64_t max_lat       = 0;
	uint64_t sum_lat       = 0;
	uint64_t min_lat1      = UINT64_MAX;
	uint64_t max_lat1      = 0;
	uint64_t sum_lat1      = 0;
	uint64_t min_lat2      = UINT64_MAX;
	uint64_t max_lat2      = 0;
	uint64_t sum_lat2      = 0;
	int      n_neg_h2      = 0;   /* rx_ns < replicator_ns: replicator clock leads destination */
	int      n_neg_total   = 0;   /* rx_ns < tx_ns: destination clock behind source (skew) -> total clamped to 0 */
	bool     has_replicator_ts = false;
	bool     has_tx_ts     = false;  /* replicator_tx_ns present -> hop2 sub-split available */

	printf("AF_XDP listening on %s queue %d  inner UDP dst port=%d  "
	       "expect=%d  timeout=%ds\n\n",
	       iface, queue, port, count, timeout);

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	struct pollfd pfd = { xsk_fd, POLLIN, 0 };

	/* ── RX loop ──────────────────────────────────────────────────────── */
	while (received < count && !g_stop) {
		uint32_t idx_rx = 0;
		uint32_t rcvd   = xsk_ring_cons__peek(&rx, BATCH, &idx_rx);

		if (rcvd == 0) {
			if (xsk_ring_prod__needs_wakeup(&fq))
				poll(&pfd, 1, timeout * 1000);
			continue;
		}

		for (uint32_t i = 0; i < rcvd && received < count; i++) {
			const struct xdp_desc *desc =
				xsk_ring_cons__rx_desc(&rx, idx_rx + i);
			uint64_t rx_ns = now_ns();

			const uint8_t *pkt = (const uint8_t*)
				xsk_umem__get_data(g_umem_buf, desc->addr);
			uint32_t len = desc->len;

			/* ── Ethernet ───────────────────────────────────────── */
			if (len < sizeof(struct ethhdr)) goto next;
			{
			const auto *eth = reinterpret_cast<const struct ethhdr *>(pkt);
			if (ntohs(eth->h_proto) != ETH_P_IPV4) goto next;

			/* ── IPv4 ───────────────────────────────────────────── */
			size_t off = sizeof(struct ethhdr);
			if (len < off + sizeof(struct iphdr)) goto next;
			const auto *ip =
				reinterpret_cast<const struct iphdr *>(pkt + off);
			if (ip->protocol != IPPROTO_UDP) goto next;
			size_t ip_len = (size_t)ip->ihl * 4;
			if (ip_len < 20 || len < off + ip_len) goto next;
			off += ip_len;

			/* ── UDP ────────────────────────────────────────────── */
			if (len < off + sizeof(struct udphdr)) goto next;
			const auto *udp =
				reinterpret_cast<const struct udphdr *>(pkt + off);
			if (ntohs(udp->dest) != (uint16_t)port) goto next;
			off += sizeof(struct udphdr);

			/* ── m2u tunnel header: magic(4) + group(4) ─────────── */
			if (len < off + (size_t)M2U_HDR_LEN) goto next;
			{
				uint32_t magic;
				memcpy(&magic, pkt + off, 4);
				if (ntohl(magic) != M2U_MAGIC) goto next;
			}
			off += M2U_HDR_LEN;

			/* ── Payload ────────────────────────────────────────── */
			if (len < off + (size_t)HDR_SIZE) goto next;
			const auto *hdr =
				reinterpret_cast<const pkt_hdr *>(pkt + off);

			uint64_t seq       = betoh64_(hdr->seq);
			uint64_t tx_ns     = betoh64_(hdr->ts_ns);
			uint64_t replicator_ns = betoh64_(hdr->replicator_ns);
			uint64_t replicator_tx_ns = betoh64_(hdr->replicator_tx_ns);

			uint64_t ulat = (rx_ns >= tx_ns) ? (rx_ns - tx_ns) : 0;
			if (rx_ns < tx_ns) n_neg_total++;   /* clock skew: dest behind source */
			latencies.push_back(ulat);
			sum_lat += ulat;
			received++;
			if (ulat < min_lat) min_lat = ulat;
			if (ulat > max_lat) max_lat = ulat;

			if (replicator_ns != 0) {
				has_replicator_ts = true;
				uint64_t h1 = (replicator_ns >= tx_ns) ? (replicator_ns - tx_ns) : 0;
				latencies_hop1.push_back(h1);
				sum_lat1 += h1;  if (h1 < min_lat1) min_lat1 = h1;  if (h1 > max_lat1) max_lat1 = h1;

				int64_t h2_signed = (int64_t)rx_ns - (int64_t)replicator_ns;
				if (h2_signed <= 0) {
					n_neg_h2++;
				} else {
					uint64_t h2 = (uint64_t)h2_signed;
					latencies_hop2.push_back(h2);
					sum_lat2 += h2;  if (h2 < min_lat2) min_lat2 = h2;  if (h2 > max_lat2) max_lat2 = h2;

					/* hop2 sub-split — needs a sane replicator_tx_ns between
					 * replicator_ns and rx_ns. proc = relay build+submit;
					 * leg2 = wire + destination RX. */
					if (replicator_tx_ns > replicator_ns && rx_ns >= replicator_tx_ns) {
						has_tx_ts = true;
						latencies_proc.push_back(replicator_tx_ns - replicator_ns);
						latencies_leg2.push_back(rx_ns - replicator_tx_ns);
					}
				}
			}

			int64_t iseq = (int64_t)seq;
			if (last_seq >= 0) {
				if (iseq < last_seq)          ooo++;
				else if (iseq > last_seq + 1) lost += (int)(iseq - last_seq - 1);
			}
			if (iseq > last_seq) last_seq = iseq;

			if (received % 100 == 0) {
				uint64_t avg100 = 0;
				int start = (int)latencies.size() - 100;
				if (start < 0) start = 0;
				for (size_t j = (size_t)start; j < latencies.size(); j++)
					avg100 += latencies[j];
				avg100 /= (latencies.size() - (size_t)start);
				printf("  [%d/%d] last=%.1fus avg(100)=%.1fus\r",
				       received, count,
				       ulat / 1000.0, avg100 / 1000.0);
				fflush(stdout);
			}
			}
next:
			/* return frame to fill ring */
			{
			uint32_t fi = 0;
			if (xsk_ring_prod__reserve(&fq, 1, &fi) == 1) {
				*xsk_ring_prod__fill_addr(&fq, fi) = desc->addr;
				xsk_ring_prod__submit(&fq, 1);
			}
			}
		}

		xsk_ring_cons__release(&rx, rcvd);
	}

	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	if (received == 0) { printf("\nNo packets received.\n"); return 1; }

	std::sort(latencies.begin(), latencies.end());
	std::sort(latencies_hop1.begin(), latencies_hop1.end());
	std::sort(latencies_hop2.begin(), latencies_hop2.end());
	std::sort(latencies_proc.begin(), latencies_proc.end());
	std::sort(latencies_leg2.begin(), latencies_leg2.end());
	uint64_t avg_lat = sum_lat / (uint64_t)received;

	static const int pcts[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 99};

	printf("\n\n");
	printf("==================================================\n");
	printf("  Multicast Latency Report (AF_XDP)\n");
	printf("==================================================\n");
	printf("  Interface:     %s  queue %d\n", iface, queue);
	printf("  Received:      %d/%d packets (%.1fs)\n", received, count, elapsed);
	printf("  Lost:          %d packets\n", lost);
	printf("  Out of order:  %d\n", ooo);

	if (has_replicator_ts) {
		uint64_t n1 = (uint64_t)latencies_hop1.size();
		uint64_t n2 = (uint64_t)latencies_hop2.size();

		printf("\n  Hop 1 — Exchange → Feeder (usec):\n");
		printf("    Min: %8.1f   Avg: %8.1f   Max: %8.1f\n",
		       min_lat1 / 1000.0, (n1 ? sum_lat1 / n1 : 0) / 1000.0, max_lat1 / 1000.0);
		for (int p : pcts)
			printf("    P%-3d  %8.1f\n", p, pct(latencies_hop1, p) / 1000.0);

		printf("\n  Hop 2 — Feeder → Destination (usec):\n");
		if (n_neg_h2 > 0) {
			int total_h2 = n_neg_h2 + (int)latencies_hop2.size();
			printf("    [!] %d/%d samples negative (replicator clock leads destination by > transit time)\n",
			       n_neg_h2, total_h2);
			printf("        Ensure refclock PHC /dev/ptp0 is active on all nodes (phc_enable=1).\n");
		}
		if (!latencies_hop2.empty()) {
			printf("    Min: %8.1f   Avg: %8.1f   Max: %8.1f\n",
			       min_lat2 / 1000.0, (n2 ? sum_lat2 / n2 : 0) / 1000.0, max_lat2 / 1000.0);
			for (int p : pcts)
				printf("    P%-3d  %8.1f\n", p, pct(latencies_hop2, p) / 1000.0);
		} else {
			printf("    No valid samples — all negative (clock skew > hop2 transit time)\n");
		}

		if (has_tx_ts && !latencies_proc.empty()) {
			printf("\n  Hop 2 split (a) — Feeder processing: parse+build+submit (usec):\n");
			for (int p : pcts)
				printf("    P%-3d  %8.1f\n", p, pct(latencies_proc, p) / 1000.0);
			printf("\n  Hop 2 split (b) — wire + destination RX (usec):\n");
			for (int p : pcts)
				printf("    P%-3d  %8.1f\n", p, pct(latencies_leg2, p) / 1000.0);
		}
	}

	printf("\n  Total — Exchange → Destination (usec):%s\n",
	       has_replicator_ts ? "" : "  (no replicator timestamps; single-hop view)");
	printf("    Min: %8.1f   Avg: %8.1f   Max: %8.1f\n",
	       min_lat / 1000.0, avg_lat / 1000.0, max_lat / 1000.0);
	for (int p : pcts)
		printf("    P%-3d  %8.1f\n", p, pct(latencies, p) / 1000.0);
	if (n_neg_total > 0)
		printf("  ** CLOCK SKEW: %d/%d samples had rx<tx (clamped to 0) — the destination\n"
		       "     clock is BEHIND the source; one-way latency is INVALID. Re-sync chrony\n"
		       "     (chronyc makestep) on both nodes and re-run. (This is NOT a datapath fault.)\n",
		       n_neg_total, received);
	printf("==================================================\n");

	// Emit a JSON result compatible with report/gen/report.py (service_rtt_us
	// schema). The primary metric is the one-way source->destination latency;
	// hop1/hop2 are included as extras (ignored by the matrix builder).
	if (json_path) {
		FILE *jf = fopen(json_path, "w");
		if (!jf) {
			fprintf(stderr, "warning: cannot open %s: %s\n", json_path, strerror(errno));
		} else {
			uint64_t p999 = latencies.empty() ? 0
			              : latencies[(latencies.size() - 1) * 999 / 1000];
			int total_pkts = received + lost;
			double loss_pct = total_pkts > 0 ? 100.0 * lost / total_pkts : 0.0;
			fprintf(jf, "{\n");
			fprintf(jf, "  \"messages\": %d,\n", received);
			fprintf(jf, "  \"lost\": %d,\n", lost);
			fprintf(jf, "  \"loss_pct\": %.4f,\n", loss_pct);
			fprintf(jf, "  \"clock_skew_samples\": %d,\n", n_neg_total);
			fprintf(jf, "  \"timestamp_rx\": \"xdp_afxdp\",\n");
			fprintf(jf, "  \"timestamp_tx\": \"clock_realtime\",\n");
			fprintf(jf, "  \"service_rtt_us\": {\n");
			fprintf(jf, "    \"min\": %" PRIu64 ",\n", min_lat / 1000);
			fprintf(jf, "    \"mean\": %" PRIu64 ",\n", avg_lat / 1000);
			fprintf(jf, "    \"p50\": %" PRIu64 ",\n", pct(latencies, 50) / 1000);
			fprintf(jf, "    \"p90\": %" PRIu64 ",\n", pct(latencies, 90) / 1000);
			fprintf(jf, "    \"p95\": %" PRIu64 ",\n", pct(latencies, 95) / 1000);
			fprintf(jf, "    \"p99\": %" PRIu64 ",\n", pct(latencies, 99) / 1000);
			fprintf(jf, "    \"p999\": %" PRIu64 ",\n", p999 / 1000);
			fprintf(jf, "    \"max\": %" PRIu64 "\n", max_lat / 1000);
			fprintf(jf, "  },\n");
			if (has_replicator_ts && !latencies_hop1.empty()) {
				fprintf(jf, "  \"hop1_us\": { \"p50\": %" PRIu64 ", \"p99\": %" PRIu64 " },\n",
				        pct(latencies_hop1, 50) / 1000, pct(latencies_hop1, 99) / 1000);
			}
			if (has_replicator_ts && !latencies_hop2.empty()) {
				fprintf(jf, "  \"hop2_us\": { \"p50\": %" PRIu64 ", \"p99\": %" PRIu64 " },\n",
				        pct(latencies_hop2, 50) / 1000, pct(latencies_hop2, 99) / 1000);
			}
			if (has_tx_ts && !latencies_proc.empty()) {
				fprintf(jf, "  \"hop2_proc_ns\": { \"p50\": %" PRIu64 ", \"p99\": %" PRIu64 " },\n",
				        pct(latencies_proc, 50), pct(latencies_proc, 99));
				fprintf(jf, "  \"hop2_wire_ns\": { \"p50\": %" PRIu64 ", \"p99\": %" PRIu64 " },\n",
				        pct(latencies_leg2, 50), pct(latencies_leg2, 99));
			}
			fprintf(jf, "  \"received\": %d\n", received);
			fprintf(jf, "}\n");
			fclose(jf);
			printf("  JSON results: %s\n", json_path);
		}
	}

	if (raw) {
		printf("\nRaw latencies (ns):\n");
		for (size_t i = 0; i < latencies.size(); i++)
			printf("  %zu: %" PRIu64 "\n", i, latencies[i]);
	}

	return 0;
}
