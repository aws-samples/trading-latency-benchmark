/*
 * AF_XDP TX zero-copy multicast sender (light m2u tunnel).
 *
 * All packet headers are built once into every UMEM frame at startup.
 * Per packet the hot path writes only 16 bytes (seq + ts_ns) in-place,
 * stamps ts_ns immediately before xsk_ring_prod__submit, then uses an
 * sfence to order stores before the NIC DMA read.
 *
 * Compared to the AF_PACKET PACKET_TX_RING path this eliminates:
 *   - full packet build (memset + ~80 B header writes) on every packet
 *   - IP checksum recomputation (id fixed at 0 per RFC 6864)
 *   - kernel dev_queue_xmit path (~1-2 µs)
 * reducing the stamp-to-wire gap from ~3-10 µs to ~1-2 µs.
 *
 * Packet layout (plain unicast UDP + 8-byte m2u tunnel header):
 *   Eth | IPv4 (src=local, dst=replicator) | UDP |
 *   m2u { magic, group } | payload
 *
 * Flat framing: the XDP filter parses Eth/IP/UDP + the 8-byte m2u tag — no
 * outer-IP proto-47, no variable tunnel header, no inner IP.
 *
 * Required flag: -D <replicator-ip>  (unicast tunnel destination)
 * Interface (-I) must be the real NIC (e.g. eth0).
 *
 * Requires root (CAP_NET_ADMIN for AF_XDP).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <vector>

#include <arpa/inet.h>
#include "common/wire.h"   // S1: single source of the on-wire layout
#include "common/nexthop.h"  // next-hop MAC (gateway when off-subnet)
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sched.h>

#include <xdp/xsk.h>
#include <bpf/libbpf.h>

/* ── UMEM / TX ring geometry ──────────────────────────────────────────── */
static constexpr uint32_t UMEM_FRAME_SIZE = 4096;
static constexpr uint32_t UMEM_NUM_FRAMES = 256;
static constexpr uint32_t TX_RING_SIZE    = 256;

/* ── packet constants ─────────────────────────────────────────────────── */
static constexpr const char *DEF_IFACE       = "eth0";
static constexpr const char *DEF_GROUP       = "224.0.31.50";
static constexpr int         DEF_PORT        = 5000;
static constexpr int         DEF_COUNT       = 10000;
static constexpr int         DEF_INTERVAL_US = 1000;
static constexpr int         DEF_SIZE        = 64;
static constexpr int         DEF_TX_QUEUE    = 1;   /* queue 0 is RSS-pinned (carries SSH/ctrl); bind TX off it */
static constexpr int         HDR_SIZE        = WIRE_APP_HDR_LEN;   /* seq(8) + ts_ns(8) + replicator_ns(8) + replicator_tx_ns(8) */

/* Light mcast->ucast tunnel tag ("M2CU"): an 8-byte header {magic, group}
 * prepended to the UDP payload. Kept in
 * sync with src/xdp/mcast.c, src/Replicator.cpp and tools/mcast_receive.cpp. */
static constexpr uint32_t    M2U_MAGIC       = WIRE_M2U_MAGIC;
static constexpr int         M2U_HDR_LEN     = WIRE_M2U_HDR_LEN;   /* magic(4) + group(4) */

/*
 * Fixed offsets within the ethernet frame for the fields updated per packet.
 * All are uint64_t stored big-endian.
 *
 *   Eth(14) + IPv4(20) + UDP(8) + m2u(8) = 50 B
 *   payload[0..7]   = seq
 *   payload[8..15]  = ts_ns     (sender stamp, written by sender hot path)
 *   payload[16..23] = replicator_ns (replicator RX stamp, written by Replicator;
 *                                zeroed in template — receiver skips hop
 *                                breakdown if still 0)
 */
static constexpr int PAYLOAD_OFF    = WIRE_PAYLOAD_OFF;
static constexpr int SEQ_OFF        = PAYLOAD_OFF;
static constexpr int TS_OFF         = PAYLOAD_OFF + WIRE_APP_TS_NS_OFF;
static constexpr int REPLICATOR_TS_OFF  = PAYLOAD_OFF + WIRE_APP_REPL_NS_OFF;  /* written by replicator, not sender */
static constexpr int REPLICATOR_TX_TS_OFF = PAYLOAD_OFF + WIRE_APP_REPL_TX_NS_OFF;  /* written by replicator at TX submit, not sender */

struct __attribute__((packed)) pkt_hdr {
	uint64_t seq;
	uint64_t ts_ns;
	uint64_t replicator_ns;     /* 0 until Replicator overwrites at RX entry */
	uint64_t replicator_tx_ns;  /* 0 until Replicator overwrites just before TX submit */
};

/* ── helpers ──────────────────────────────────────────────────────────── */
static inline uint64_t htobe64_(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(v);
#else
	return v;
#endif
}

static inline uint64_t now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Absolute-deadline pace, matching rtt.cpp's wait_until_ns(). Waits until
// deadline_ns on CLOCK_MONOTONIC, or returns immediately if already past.
//
// This replaces per-iteration RELATIVE waits (sleep/spin for interval_ns
// measured from when the wait itself starts, i.e. AFTER that packet's ring
// reserve/memcpy/submit/doorbell work). A relative wait adds the loop's own
// processing time on top of interval_ns every iteration, so actual spacing is
// interval_ns + processing_time, not interval_ns - live measurement at
// interval_us=10 (100k pps requested) found only 65-90k pps achieved (10-35%
// short) across every fwd mode, a consistent shortfall matching this
// mechanism rather than a fwd-mode-specific cost. Pacing off an absolute
// deadline computed once per iteration (t0 + seq*interval_ns) absorbs
// processing time into the interval instead of adding to it.
//
// clock_nanosleep(TIMER_ABSTIME) is also not just cheaper than a tight spin:
// per rtt.cpp's wait_until_ns comment, sleeping (not spinning) is what lets
// this core run NAPI/softirq TX-completion cleanup between packets - a tight
// userspace spin never yields, starving that path on a zero-copy TX ring.
static inline void wait_until_ns(int64_t deadline_ns)
{
	struct timespec d;
	d.tv_sec  = deadline_ns / 1000000000LL;
	d.tv_nsec = deadline_ns % 1000000000LL;
	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, nullptr);
}

/* ── IP checksum ──────────────────────────────────────────────────────── */
static uint16_t ip_csum(const void *data, int len)
{
	const uint16_t *p = (const uint16_t *)data;
	uint32_t sum = 0;
	for (; len > 1; len -= 2) sum += *p++;
	if (len) sum += *(const uint8_t *)p;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

/* ── interface info ───────────────────────────────────────────────────── */
struct iface_info {
	uint8_t  mac[6];
	uint32_t ip;       /* host byte order */
	int      ifindex;
};

static bool get_iface_info(const char *iface, iface_info &out)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) { perror("socket"); return false; }

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

	if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) { perror("SIOCGIFHWADDR"); close(s); return false; }
	memcpy(out.mac, ifr.ifr_hwaddr.sa_data, 6);

	if (ioctl(s, SIOCGIFADDR, &ifr) < 0)   { perror("SIOCGIFADDR");   close(s); return false; }
	out.ip = ntohl(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr);

	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0)  { perror("SIOCGIFINDEX");  close(s); return false; }
	out.ifindex = ifr.ifr_ifindex;

	close(s);
	return true;
}

/* ── ARP MAC resolution ───────────────────────────────────────────────── */

/*
 * Build the full m2u packet template into buf.
 *
 * outer->id is set to 0: with IP_DF set fragmentation is impossible so the
 * ID field is meaningless (RFC 6864 §4.1).  This makes the outer IP header
 * fully static — its checksum is computed once here and never recalculated.
 *
 * seq and ts_ns are zeroed as placeholders.  The hot path overwrites them
 * at SEQ_OFF and TS_OFF respectively without touching any other field.
 */
static int build_m2u_pkt(uint8_t *buf, int max_buf,
                          const iface_info &src,
                          const uint8_t dst_mac[6],
                          uint32_t replicator_ip_nbo,
                          uint32_t mcast_ip_nbo,
                          uint16_t udp_dst_port,
                          int      payload_size)
{
	const int m2u_len  = M2U_HDR_LEN;                       /* magic(4) + group(4) */
	const int udp_len  = (int)sizeof(struct udphdr) + m2u_len + payload_size;
	const int ip_len   = (int)sizeof(struct iphdr)  + udp_len;
	const int eth_len  = (int)sizeof(struct ethhdr);
	const int total    = eth_len + ip_len;

	if (total > max_buf) return -1;
	memset(buf, 0, total);

	uint8_t *p = buf;

	/* Ethernet */
	auto *eth = reinterpret_cast<struct ethhdr *>(p);
	memcpy(eth->h_dest,   dst_mac,  6);
	memcpy(eth->h_source, src.mac,  6);
	eth->h_proto = htons(ETH_P_IP);
	p += sizeof(struct ethhdr);

	/* IPv4 — plain unicast to the replicator; id=0, checksum stable for the run */
	auto *ip = reinterpret_cast<struct iphdr *>(p);
	ip->ihl      = 5;
	ip->version  = 4;
	ip->tos      = 0;
	ip->tot_len  = htons((uint16_t)ip_len);
	ip->id       = 0;
	ip->frag_off = htons(IP_DF);
	ip->ttl      = 64;
	ip->protocol = IPPROTO_UDP;
	ip->saddr    = htonl(src.ip);
	ip->daddr    = replicator_ip_nbo;
	ip->check    = ip_csum(ip, sizeof(struct iphdr));
	p += sizeof(struct iphdr);

	/* UDP */
	auto *udp   = reinterpret_cast<struct udphdr *>(p);
	udp->source = htons(60000);
	udp->dest   = udp_dst_port;
	udp->len    = htons((uint16_t)udp_len);
	udp->check  = 0;               /* UDP checksum optional over IPv4 */
	p += sizeof(struct udphdr);

	/* m2u tunnel header: magic + multicast group (both network byte order) */
	uint32_t magic_be = htonl(M2U_MAGIC);
	memcpy(p,     &magic_be,     4);
	memcpy(p + 4, &mcast_ip_nbo, 4);   /* group already in network order */
	p += m2u_len;

	/* payload bytes [0..15] left as zero (seq=0, ts_ns=0 placeholders) */

	return total;
}

/* ── kernel mode (-k): plain UDP socket, no AF_XDP/root ──────────────────
 * sendto() only needs [m2u(8) | app payload] — the kernel builds Eth/IP/UDP
 * itself, unlike the AF_XDP path above which builds the full raw frame.
 * Same wire.h layout, same replicator_ns/replicator_tx_ns semantics; this is
 * the apples-to-apples TX-side counterpart of REPLICATOR_FWD_MODE=kernel. */
static int run_kernel_send(const char *replicator_ip_s, const char *group, int port,
                            int count, int interval_us, int pkt_size)
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) { perror("socket"); return 1; }

	struct sockaddr_in dst{};
	dst.sin_family = AF_INET;
	dst.sin_port   = htons((uint16_t)port);
	if (inet_pton(AF_INET, replicator_ip_s, &dst.sin_addr) != 1) {
		fprintf(stderr, "error: invalid replicator IP %s\n", replicator_ip_s);
		close(sock);
		return 1;
	}

	uint32_t mcast_ip_nbo;
	inet_pton(AF_INET, group, &mcast_ip_nbo);

	const int m2u_len = M2U_HDR_LEN;
	const int buf_len = m2u_len + pkt_size;
	std::vector<uint8_t> buf(buf_len, 0);

	uint32_t magic_be = htonl(M2U_MAGIC);
	memcpy(buf.data(),     &magic_be,     4);
	memcpy(buf.data() + 4, &mcast_ip_nbo, 4);
	/* payload bytes [0..15] (seq, ts_ns) are overwritten per packet below;
	 * [16..31] (replicator_ns, replicator_tx_ns) stay zero — the receiver
	 * treats a still-zero replicator_ns as "no hop breakdown available". */

	printf("Sending %d packets to replicator %s (inner %s:%d)  "
	       "payload=%dB  interval=%dus  [kernel mode]\n\n",
	       count, replicator_ip_s, group, port, pkt_size, interval_us);

	uint64_t interval_ns = (uint64_t)interval_us * 1000ULL;

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int64_t t0_ns = (int64_t)t0.tv_sec * 1000000000LL + t0.tv_nsec;

	for (int seq = 0; seq < count; seq++) {
		uint64_t seq_be = htobe64_((uint64_t)seq);
		memcpy(buf.data() + m2u_len + WIRE_APP_SEQ_OFF, &seq_be, 8);

		/* Stamp ts_ns immediately before sendto(), mirroring the AF_XDP path's
		 * "stamp right before submit" placement (there submit = ring push,
		 * here submit = the sendto() call itself). */
		uint64_t ts_be = htobe64_(now_ns());
		memcpy(buf.data() + m2u_len + WIRE_APP_TS_NS_OFF, &ts_be, 8);

		ssize_t sent = sendto(sock, buf.data(), buf.size(), 0,
		                      reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));
		if (sent < 0) {
			perror("sendto");
		}

		if (seq % 100 == 0) {
			printf("  sent %d/%d\r", seq, count);
			fflush(stdout);
		}

		if (interval_us > 0)
			wait_until_ns(t0_ns + (int64_t)(seq + 1) * (int64_t)interval_ns);
	}

	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	double achieved_pps = elapsed > 0 ? count / elapsed : 0.0;
	double requested_pps = interval_us > 0 ? 1e6 / interval_us : achieved_pps;
	printf("\nDone. Sent %d packets in %.3fs (achieved %.0f pps, requested %.0f pps, %.1f%%)  [kernel mode]\n",
	       count, elapsed, achieved_pps, requested_pps,
	       requested_pps > 0 ? 100.0 * achieved_pps / requested_pps : 0.0);
	close(sock);
	return 0;
}

/* ── real-time scheduling ────────────────────────────────────────────────
 * AF_XDP-path-only: matches rtt.cpp's enable_realtime(). SCHED_FIFO removes
 * scheduler wakeup latency on the TX hot loop (stamp -> submit -> doorbell),
 * the same class of cost that made hop1 rate-sensitive (see dev/roadmap/fix.md's
 * hop1 TX-doorbell finding). Not applied in run_kernel_send(): that path is
 * the deliberately-untuned stock-kernel baseline (dev/roadmap/fix.md item 4) and
 * must stay that way for the comparison to mean what it claims to. */
static void enable_realtime()
{
	struct sched_param param = {};
	param.sched_priority = 80;
	if (sched_setscheduler(0, SCHED_FIFO, &param) == 0)
		printf("  SCHED_FIFO priority 80 enabled\n");
	else
		fprintf(stderr, "  Warning: SCHED_FIFO failed (need root/CAP_SYS_NICE), continuing with SCHED_OTHER\n");

	if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
		printf("  mlockall enabled (no page faults during measurement)\n");
	else
		fprintf(stderr, "  Warning: mlockall failed (need root/CAP_IPC_LOCK)\n");
}

/* ── usage ────────────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
	printf("Usage: %s [options]\n"
	       "  -I <iface>       real NIC interface        (default: %s)\n"
	       "  -D <replicator-ip>   replicator IP (unicast tunnel dst) (REQUIRED)\n"
	       "  -g <group>       multicast group (carried in m2u hdr) (default: %s)\n"
	       "  -p <port>        UDP dst port              (default: %d)\n"
	       "  -c <count>       number of packets         (default: %d)\n"
	       "  -i <interval_us> inter-packet gap µs       (default: %d)\n"
	       "  -s <size>        payload bytes             (default: %d, min: %d)\n"
	       "  -q <queue>       AF_XDP TX queue           (default: %d, ignored with -k)\n"
	       "  -k               kernel mode: plain UDP socket, no AF_XDP/root\n"
	       "                   (apples-to-apples baseline vs REPLICATOR_FWD_MODE=kernel)\n"
	       "  -h               this help\n",
	       prog, DEF_IFACE, DEF_GROUP, DEF_PORT,
	       DEF_COUNT, DEF_INTERVAL_US, DEF_SIZE, HDR_SIZE, DEF_TX_QUEUE);
}

int main(int argc, char *argv[])
{
	const char *iface       = DEF_IFACE;
	const char *replicator_ip_s = nullptr;
	const char *group       = DEF_GROUP;
	int port        = DEF_PORT;
	int count       = DEF_COUNT;
	int interval_us = DEF_INTERVAL_US;
	int pkt_size    = DEF_SIZE;
	int tx_queue    = DEF_TX_QUEUE;

	bool kernel_mode = false;

	int opt;
	while ((opt = getopt(argc, argv, "I:D:g:p:c:i:s:q:kh")) != -1) {
		switch (opt) {
		case 'I': iface       = optarg;          break;
		case 'D': replicator_ip_s = optarg;          break;
		case 'g': group       = optarg;          break;
		case 'p': port        = atoi(optarg);    break;
		case 'c': count       = atoi(optarg);    break;
		case 'i': interval_us = atoi(optarg);    break;
		case 's': pkt_size    = atoi(optarg);    break;
		case 'q': tx_queue    = atoi(optarg);    break;
		case 'k': kernel_mode = true;             break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	if (!replicator_ip_s) {
		fprintf(stderr, "error: -D <replicator-ip> is required\n");
		usage(argv[0]);
		return 1;
	}
	if (pkt_size < HDR_SIZE) pkt_size = HDR_SIZE;

	if (kernel_mode)
		return run_kernel_send(replicator_ip_s, group, port, count, interval_us, pkt_size);

	/* ── interface info ───────────────────────────────────────────────── */
	iface_info src;
	if (!get_iface_info(iface, src)) return 1;

	/* ── resolve replicator MAC ───────────────────────────────────────────── */
	uint8_t dst_mac[6];
	printf("Resolving MAC for %s ...\n", replicator_ip_s);
	if (!afxdp::resolve_next_hop_mac(replicator_ip_s, iface, dst_mac)) {
		fprintf(stderr, "error: next-hop MAC resolution failed for %s\n", replicator_ip_s);
		fprintf(stderr, "       ensure replicator is reachable and try again\n");
		return 1;
	}
	printf("  replicator MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       dst_mac[0], dst_mac[1], dst_mac[2],
	       dst_mac[3], dst_mac[4], dst_mac[5]);

	/* ── network-order IPs ────────────────────────────────────────────── */
	uint32_t replicator_ip_nbo, mcast_ip_nbo;
	inet_pton(AF_INET, replicator_ip_s, &replicator_ip_nbo);
	inet_pton(AF_INET, group,       &mcast_ip_nbo);
	uint16_t dst_port_nbo = htons((uint16_t)port);

	/* ── AF_XDP setup ─────────────────────────────────────────────────── */
	{
		struct rlimit r = { RLIM_INFINITY, RLIM_INFINITY };
		if (setrlimit(RLIMIT_MEMLOCK, &r) < 0)
			perror("setrlimit RLIMIT_MEMLOCK (continuing)");
	}

	/* Allocate page-aligned UMEM */
	void *umem_buf = nullptr;
	if (posix_memalign(&umem_buf, getpagesize(),
	                   (size_t)UMEM_NUM_FRAMES * UMEM_FRAME_SIZE) != 0) {
		perror("posix_memalign"); return 1;
	}

	/*
	 * Pre-build the packet template into frame 0, then replicate it into
	 * all remaining frames.  Every frame is now ready to transmit; the hot
	 * path only overwrites the 16 bytes at SEQ_OFF / TS_OFF.
	 */
	int pkt_len = build_m2u_pkt(
		(uint8_t *)umem_buf, UMEM_FRAME_SIZE,
		src, dst_mac, replicator_ip_nbo, mcast_ip_nbo,
		dst_port_nbo, pkt_size);
	if (pkt_len < 0) {
		fprintf(stderr, "error: packet size exceeds UMEM frame (%u bytes)\n",
		        UMEM_FRAME_SIZE);
		return 1;
	}
	for (uint32_t i = 1; i < UMEM_NUM_FRAMES; i++)
		memcpy((uint8_t *)umem_buf + i * UMEM_FRAME_SIZE, umem_buf, pkt_len);

	/* Create UMEM — TX-only: fill ring unused, completion ring active */
	struct xsk_ring_prod fq_unused{};
	struct xsk_ring_cons cq{};
	struct xsk_umem *umem = nullptr;
	const struct xsk_umem_config umem_cfg = {
		.fill_size      = TX_RING_SIZE,
		.comp_size      = TX_RING_SIZE,
		.frame_size     = UMEM_FRAME_SIZE,
		.frame_headroom = 0,
		.flags          = 0,
	};
	int err = xsk_umem__create(&umem, umem_buf,
	                           (uint64_t)UMEM_NUM_FRAMES * UMEM_FRAME_SIZE,
	                           &fq_unused, &cq, &umem_cfg);
	if (err) { fprintf(stderr, "xsk_umem__create: %s\n", strerror(-err)); return 1; }

	/* Create AF_XDP socket — TX only, no XDP program load */
	struct xsk_ring_prod tx_ring{};
	struct xsk_socket *xsk = nullptr;
	struct xsk_socket_config xsk_cfg = {
		.rx_size      = 0,
		.tx_size      = TX_RING_SIZE,
		.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
		.xdp_flags    = XDP_FLAGS_DRV_MODE,   /* native ZC; falls back to SKB */
		.bind_flags   = XDP_USE_NEED_WAKEUP,
	};
	err = xsk_socket__create(&xsk, iface, tx_queue, umem, nullptr, &tx_ring, &xsk_cfg);
	if (err) {
		fprintf(stderr, "native XDP failed (%s), trying SKB mode\n", strerror(-err));
		xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
		err = xsk_socket__create(&xsk, iface, tx_queue, umem, nullptr, &tx_ring, &xsk_cfg);
	}
	if (err) { fprintf(stderr, "xsk_socket__create: %s\n", strerror(-err)); return 1; }
	int xsk_fd = xsk_socket__fd(xsk);

	printf("Sending %d packets to replicator %s (inner %s:%d) via %s q%d  "
	       "payload=%dB  interval=%dus\n\n",
	       count, replicator_ip_s, group, port, iface, tx_queue, pkt_size, interval_us);

	// SCHED_FIFO + mlockall from here, not from before AF_XDP setup: MAC
	// resolution, posix_memalign, xsk_umem__create and xsk_socket__create all
	// make syscalls that can need the kernel to schedule ordinary work on this
	// core. Running that setup at RT priority with pages already locked risks
	// starving whatever the kernel needs to finish those calls - measured
	// live on mcast_receive.cpp's equivalent call site as ~50% of runs losing
	// 40-70% of packets when placed before setup instead of here. Enabling RT
	// only around the steady-state hot loop avoids that risk entirely.
	enable_realtime();

	uint64_t interval_ns = (uint64_t)interval_us * 1000ULL;
	uint32_t outstanding = 0;

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int64_t t0_ns = (int64_t)t0.tv_sec * 1000000000LL + t0.tv_nsec;

	for (int seq = 0; seq < count; seq++) {
		/* drain completions */
		uint32_t comp_idx = 0;
		uint32_t completed = xsk_ring_cons__peek(&cq, TX_RING_SIZE, &comp_idx);
		if (completed > 0) {
			xsk_ring_cons__release(&cq, completed);
			outstanding -= completed;
		}

		/* reserve TX descriptor — spin if ring full */
		uint32_t tx_idx = 0;
		while (xsk_ring_prod__reserve(&tx_ring, 1, &tx_idx) == 0) {
			if (xsk_ring_prod__needs_wakeup(&tx_ring))
				sendto(xsk_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
			completed = xsk_ring_cons__peek(&cq, TX_RING_SIZE, &comp_idx);
			if (completed > 0) {
				xsk_ring_cons__release(&cq, completed);
				outstanding -= completed;
			}
		}

		/* select UMEM frame (round-robin) and update seq field */
		uint64_t frame_addr = ((uint64_t)seq % UMEM_NUM_FRAMES) * UMEM_FRAME_SIZE;
		uint8_t *frame = (uint8_t *)umem_buf + frame_addr;

		uint64_t seq_be = htobe64_((uint64_t)seq);
		__builtin_memcpy(frame + SEQ_OFF, &seq_be, 8);

		/*
		 * Stamp ts_ns immediately before descriptor submit.
		 * The sfence ensures all frame stores are globally visible
		 * before the NIC DMA engine reads the descriptor.
		 */
		uint64_t ts_be = htobe64_(now_ns());
		__builtin_memcpy(frame + TS_OFF, &ts_be, 8);

		struct xdp_desc *desc = xsk_ring_prod__tx_desc(&tx_ring, tx_idx);
		desc->addr = frame_addr;
		desc->len  = (uint32_t)pkt_len;
		asm volatile("sfence" ::: "memory");
		xsk_ring_prod__submit(&tx_ring, 1);
		outstanding++;

		if (xsk_ring_prod__needs_wakeup(&tx_ring))
			sendto(xsk_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);

		// Every 10000, no fflush - see mcast_receive.cpp's identical fix for
		// why: fflush's write(2) inside a SCHED_FIFO hot loop on an isolated
		// core is an uninterruptible stall surface, measured live as
		// intermittent multi-hundred-ms latency spikes.
		if (seq % 10000 == 0) {
			printf("  sent %d/%d\n", seq, count);
		}

		if (interval_us > 0)
			wait_until_ns(t0_ns + (int64_t)(seq + 1) * (int64_t)interval_ns);
	}

	/* flush outstanding TX */
	if (xsk_ring_prod__needs_wakeup(&tx_ring))
		sendto(xsk_fd, nullptr, 0, 0, nullptr, 0);
	for (int retries = 100; retries > 0 && outstanding > 0; retries--) {
		uint32_t comp_idx = 0;
		uint32_t completed = xsk_ring_cons__peek(&cq, TX_RING_SIZE, &comp_idx);
		if (completed > 0) { xsk_ring_cons__release(&cq, completed); outstanding -= completed; }
		if (outstanding > 0) usleep(1000);
	}

	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	double achieved_pps = elapsed > 0 ? count / elapsed : 0.0;
	double requested_pps = interval_us > 0 ? 1e6 / interval_us : achieved_pps;

	printf("\nDone. Sent %d packets in %.3fs (achieved %.0f pps, requested %.0f pps, %.1f%%)\n",
	       count, elapsed, achieved_pps, requested_pps,
	       requested_pps > 0 ? 100.0 * achieved_pps / requested_pps : 0.0);
	xsk_socket__delete(xsk);
	xsk_umem__delete(umem);
	free(umem_buf);
	return 0;
}
