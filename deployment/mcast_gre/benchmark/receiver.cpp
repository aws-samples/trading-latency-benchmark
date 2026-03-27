/*
 * latency_receiver.cpp — AF_XDP GRE latency receiver.
 *
 * Attaches subscriber_filter.o to the NIC via XDP, opens an AF_XDP
 * socket on the chosen queue, and polls the RX ring directly — no
 * kernel IP stack involvement after the XDP redirect.
 *
 * Packet layout received (starting from Ethernet header):
 *   Eth | outer IPv4 | GRE (4B) | inner IPv4 | UDP | payload
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
#include <time.h>
#include <signal.h>
#include <poll.h>

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
static constexpr int      HDR_SIZE    = 24;     /* seq(8) + ts_ns(8) + feeder_ns(8) */

struct __attribute__((packed)) pkt_hdr {
	uint64_t seq;
	uint64_t ts_ns;
	uint64_t feeder_ns;  /* 0 if no feeder stamp; non-zero enables per-hop breakdown */
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
	       "  -i <iface>   network interface (required)\n"
	       "  -p <port>    inner UDP dst port to match (default: %d)\n"
	       "  -c <count>   packets to receive        (default: %d)\n"
	       "  -t <timeout> seconds before giving up  (default: %d)\n"
	       "  -q <queue>   XDP/AF_XDP queue index    (default: %d)\n"
	       "  -B <path>    path to subscriber_filter.o\n"
	       "               (default: ./subscriber_filter.o)\n"
	       "  -r           print raw latencies (ns)\n"
	       "  -h           this help\n"
	       "\nRequires root (XDP attach + AF_XDP).\n",
	       prog, DEF_PORT, DEF_COUNT, DEF_TIMEOUT, DEF_QUEUE);
}

int main(int argc, char *argv[])
{
	const char *iface    = nullptr;
	const char *bpf_path = "./subscriber_filter.o";
	int  port    = DEF_PORT;
	int  count   = DEF_COUNT;
	int  timeout = DEF_TIMEOUT;
	int  queue   = DEF_QUEUE;
	bool raw     = false;

	int opt;
	while ((opt = getopt(argc, argv, "i:p:c:t:q:B:rh")) != -1) {
		switch (opt) {
		case 'i': iface    = optarg;           break;
		case 'p': port     = atoi(optarg);     break;
		case 'c': count    = atoi(optarg);     break;
		case 't': timeout  = atoi(optarg);     break;
		case 'q': queue    = atoi(optarg);     break;
		case 'B': bpf_path = optarg;           break;
		case 'r': raw      = true;             break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}
	if (!iface) { fprintf(stderr, "error: -i <iface> is required\n"); usage(argv[0]); return 1; }

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
	    bpf_object__find_program_by_name(g_bpf_obj, "xdp_gre_redirect");
	if (!bpf_prog) {
		fprintf(stderr, "error: XDP program 'xdp_gre_redirect' not found in %s\n", bpf_path);
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
	if (bpf_map_update_elem(map_fd, &queue, &xsk_fd, BPF_ANY) < 0) {
		perror("bpf_map_update_elem(xsks_map)");
		return 1;
	}

	/* ── pre-populate fill ring ───────────────────────────────────────── */
	uint32_t fill_idx = 0;
	uint32_t fill_n   = NUM_FRAMES / 2;
	if (xsk_ring_prod__reserve(&fq, fill_n, &fill_idx) != fill_n) {
		fprintf(stderr, "error: fill ring reserve failed\n");
		return 1;
	}
	for (uint32_t i = 0; i < fill_n; i++)
		*xsk_ring_prod__fill_addr(&fq, fill_idx + i) = (uint64_t)i * FRAME_SIZE;
	xsk_ring_prod__submit(&fq, fill_n);

	/* ── stats ────────────────────────────────────────────────────────── */
	std::vector<uint64_t> latencies;       /* total: rx_ns - tx_ns */
	std::vector<uint64_t> latencies_hop1;  /* hop1:  feeder_ns - tx_ns   (exchange → feeder) */
	std::vector<uint64_t> latencies_hop2;  /* hop2:  rx_ns - feeder_ns   (feeder → subscriber) */
	latencies.reserve(count);
	latencies_hop1.reserve(count);
	latencies_hop2.reserve(count);
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
	int      n_neg_h2      = 0;   /* rx_ns < feeder_ns: feeder clock leads subscriber */
	bool     has_feeder_ts = false;

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

			/* ── Outer IPv4 ─────────────────────────────────────── */
			size_t off = sizeof(struct ethhdr);
			if (len < off + sizeof(struct iphdr)) goto next;
			const auto *outer_ip =
				reinterpret_cast<const struct iphdr *>(pkt + off);
			if (outer_ip->protocol != 47) goto next;
			size_t outer_ip_len = (size_t)outer_ip->ihl * 4;
			if (outer_ip_len < 20 || len < off + outer_ip_len + 4) goto next;
			off += outer_ip_len;

			/* ── GRE header ─────────────────────────────────────── */
			const uint8_t *gre   = pkt + off;
			uint16_t gre_flags   = (uint16_t)((gre[0] << 8) | gre[1]);
			uint16_t gre_proto   = (uint16_t)((gre[2] << 8) | gre[3]);
			if (gre_proto != ETH_P_IPV4) goto next;

			size_t gre_len = 4;
			if (gre_flags & 0x8000) gre_len += 4;
			if (gre_flags & 0x2000) gre_len += 4;
			if (gre_flags & 0x1000) gre_len += 4;
			off += gre_len;

			/* ── Inner IPv4 ─────────────────────────────────────── */
			if (len < off + sizeof(struct iphdr)) goto next;
			const auto *inner_ip =
				reinterpret_cast<const struct iphdr *>(pkt + off);
			if (inner_ip->protocol != IPPROTO_UDP) goto next;
			size_t inner_ip_len = (size_t)inner_ip->ihl * 4;
			if (inner_ip_len < 20) goto next;
			off += inner_ip_len;

			/* ── Inner UDP ──────────────────────────────────────── */
			if (len < off + sizeof(struct udphdr)) goto next;
			const auto *udp =
				reinterpret_cast<const struct udphdr *>(pkt + off);
			if (ntohs(udp->dest) != (uint16_t)port) goto next;
			off += sizeof(struct udphdr);

			/* ── Payload ────────────────────────────────────────── */
			if (len < off + (size_t)HDR_SIZE) goto next;
			const auto *hdr =
				reinterpret_cast<const pkt_hdr *>(pkt + off);

			uint64_t seq       = betoh64_(hdr->seq);
			uint64_t tx_ns     = betoh64_(hdr->ts_ns);
			uint64_t feeder_ns = betoh64_(hdr->feeder_ns);

			uint64_t ulat = (rx_ns >= tx_ns) ? (rx_ns - tx_ns) : 0;
			latencies.push_back(ulat);
			sum_lat += ulat;
			received++;
			if (ulat < min_lat) min_lat = ulat;
			if (ulat > max_lat) max_lat = ulat;

			if (feeder_ns != 0) {
				has_feeder_ts = true;
				uint64_t h1 = (feeder_ns >= tx_ns) ? (feeder_ns - tx_ns) : 0;
				latencies_hop1.push_back(h1);
				sum_lat1 += h1;  if (h1 < min_lat1) min_lat1 = h1;  if (h1 > max_lat1) max_lat1 = h1;

				int64_t h2_signed = (int64_t)rx_ns - (int64_t)feeder_ns;
				if (h2_signed <= 0) {
					n_neg_h2++;
				} else {
					uint64_t h2 = (uint64_t)h2_signed;
					latencies_hop2.push_back(h2);
					sum_lat2 += h2;  if (h2 < min_lat2) min_lat2 = h2;  if (h2 > max_lat2) max_lat2 = h2;
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
	uint64_t avg_lat = sum_lat / (uint64_t)received;

	static const int pcts[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 99};

	printf("\n\n");
	printf("==================================================\n");
	printf("  GRE Latency Report (AF_XDP)\n");
	printf("==================================================\n");
	printf("  Interface:     %s  queue %d\n", iface, queue);
	printf("  Received:      %d/%d packets (%.1fs)\n", received, count, elapsed);
	printf("  Lost:          %d packets\n", lost);
	printf("  Out of order:  %d\n", ooo);

	if (has_feeder_ts) {
		uint64_t n1 = (uint64_t)latencies_hop1.size();
		uint64_t n2 = (uint64_t)latencies_hop2.size();

		printf("\n  Hop 1 — Exchange → Feeder (usec):\n");
		printf("    Min: %8.1f   Avg: %8.1f   Max: %8.1f\n",
		       min_lat1 / 1000.0, (n1 ? sum_lat1 / n1 : 0) / 1000.0, max_lat1 / 1000.0);
		for (int p : pcts)
			printf("    P%-3d  %8.1f\n", p, pct(latencies_hop1, p) / 1000.0);

		printf("\n  Hop 2 — Feeder → Subscriber (usec):\n");
		if (n_neg_h2 > 0) {
			int total_h2 = n_neg_h2 + (int)latencies_hop2.size();
			printf("    [!] %d/%d samples negative (feeder clock leads subscriber by > transit time)\n",
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
	}

	printf("\n  Total — Exchange → Subscriber (usec):%s\n",
	       has_feeder_ts ? "" : "  (no feeder timestamps; single-hop view)");
	printf("    Min: %8.1f   Avg: %8.1f   Max: %8.1f\n",
	       min_lat / 1000.0, avg_lat / 1000.0, max_lat / 1000.0);
	for (int p : pcts)
		printf("    P%-3d  %8.1f\n", p, pct(latencies, p) / 1000.0);
	printf("==================================================\n");

	if (raw) {
		printf("\nRaw latencies (ns):\n");
		for (size_t i = 0; i < latencies.size(); i++)
			printf("  %zu: %" PRIu64 "\n", i, latencies[i]);
	}

	return 0;
}
