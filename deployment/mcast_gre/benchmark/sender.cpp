/*
 * latency_sender.cpp — AF_XDP TX zero-copy GRE sender.
 *
 * All packet headers are built once into every UMEM frame at startup.
 * Per packet the hot path writes only 16 bytes (seq + ts_ns) in-place,
 * stamps ts_ns immediately before xsk_ring_prod__submit, then uses an
 * sfence to order stores before the NIC DMA read.
 *
 * Compared to the AF_PACKET PACKET_TX_RING path this eliminates:
 *   - full packet build (memset + ~80 B header writes) on every packet
 *   - outer IP checksum recomputation (outer->id fixed at 0 per RFC 6864)
 *   - kernel dev_queue_xmit path (~1-2 µs)
 * reducing the stamp-to-wire gap from ~3-10 µs to ~1-2 µs.
 *
 * Packet layout:
 *   Eth | outer IPv4 (src=local, dst=feeder) | GRE (4B) |
 *   inner IPv4 (src=local, dst=mcast_group)  | UDP | payload
 *
 * Required flag: -D <feeder-ip>  (outer GRE destination)
 * Interface (-I) must be the real NIC (e.g. eth0), NOT gre_feed.
 *
 * Requires root (CAP_NET_ADMIN for AF_XDP).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>

#include <arpa/inet.h>
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
static constexpr int         HDR_SIZE        = 24;   /* seq(8) + ts_ns(8) + feeder_ns(8) */

/*
 * Fixed offsets within the ethernet frame for the fields updated per packet.
 * All are uint64_t stored big-endian.
 *
 *   Eth(14) + outer IPv4(20) + GRE(4) + inner IPv4(20) + UDP(8) = 66 B
 *   payload[0..7]   = seq
 *   payload[8..15]  = ts_ns     (sender stamp, written by sender hot path)
 *   payload[16..23] = feeder_ns (feeder RX stamp, written by PacketReplicator;
 *                                zeroed in template — receiver skips hop
 *                                breakdown if still 0)
 */
static constexpr int PAYLOAD_OFF    = 14 + 20 + 4 + 20 + 8;
static constexpr int SEQ_OFF        = PAYLOAD_OFF;
static constexpr int TS_OFF         = PAYLOAD_OFF + 8;
static constexpr int FEEDER_TS_OFF  = PAYLOAD_OFF + 16;  /* written by feeder, not sender */

struct __attribute__((packed)) pkt_hdr {
	uint64_t seq;
	uint64_t ts_ns;
	uint64_t feeder_ns;  /* 0 until PacketReplicator overwrites in transit */
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

static inline void busy_wait_ns(uint64_t ns)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t target = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec + ns;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec >= target) break;
	}
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
static bool resolve_mac(const char *dst_ip, const char *iface, uint8_t mac[6])
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return false;
	setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface) + 1);
	struct sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_port   = htons(9);
	inet_pton(AF_INET, dst_ip, &a.sin_addr);
	connect(s, (struct sockaddr *)&a, sizeof(a));
	send(s, nullptr, 0, 0);
	close(s);

	usleep(50000);

	FILE *f = fopen("/proc/net/arp", "r");
	if (!f) { perror("open /proc/net/arp"); return false; }

	char line[256];
	fgets(line, sizeof(line), f);
	while (fgets(line, sizeof(line), f)) {
		char ip[32], hwtype[16], flags[16], hw[32], mask[16], dev[32];
		if (sscanf(line, "%31s %15s %15s %31s %15s %31s",
		           ip, hwtype, flags, hw, mask, dev) != 6) continue;
		if (strcmp(ip, dst_ip) != 0) continue;
		unsigned int b[6];
		if (sscanf(hw, "%x:%x:%x:%x:%x:%x",
		           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) continue;
		for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
		fclose(f);
		return true;
	}
	fclose(f);
	return false;
}

/*
 * Build the full GRE packet template into buf.
 *
 * outer->id is set to 0: with IP_DF set fragmentation is impossible so the
 * ID field is meaningless (RFC 6864 §4.1).  This makes the outer IP header
 * fully static — its checksum is computed once here and never recalculated.
 *
 * seq and ts_ns are zeroed as placeholders.  The hot path overwrites them
 * at SEQ_OFF and TS_OFF respectively without touching any other field.
 */
static int build_gre_pkt(uint8_t *buf, int max_buf,
                          const iface_info &src,
                          const uint8_t dst_mac[6],
                          uint32_t feeder_ip_nbo,
                          uint32_t mcast_ip_nbo,
                          uint16_t udp_dst_port,
                          int      payload_size)
{
	const int inner_udp_len  = (int)sizeof(struct udphdr) + payload_size;
	const int inner_ip_len   = (int)sizeof(struct iphdr)  + inner_udp_len;
	const int gre_len        = 4;
	const int outer_ip_len   = (int)sizeof(struct iphdr);
	const int eth_len        = (int)sizeof(struct ethhdr);
	const int total          = eth_len + outer_ip_len + gre_len + inner_ip_len;

	if (total > max_buf) return -1;
	memset(buf, 0, total);

	uint8_t *p = buf;

	/* Ethernet */
	auto *eth = reinterpret_cast<struct ethhdr *>(p);
	memcpy(eth->h_dest,   dst_mac,  6);
	memcpy(eth->h_source, src.mac,  6);
	eth->h_proto = htons(ETH_P_IP);
	p += sizeof(struct ethhdr);

	/* Outer IPv4 — id=0, checksum stable for the entire run */
	auto *outer = reinterpret_cast<struct iphdr *>(p);
	outer->ihl      = 5;
	outer->version  = 4;
	outer->tos      = 0;
	outer->tot_len  = htons((uint16_t)(outer_ip_len + gre_len + inner_ip_len));
	outer->id       = 0;
	outer->frag_off = htons(IP_DF);
	outer->ttl      = 64;
	outer->protocol = 47;  /* GRE */
	outer->saddr    = htonl(src.ip);
	outer->daddr    = feeder_ip_nbo;
	outer->check    = ip_csum(outer, sizeof(struct iphdr));
	p += sizeof(struct iphdr);

	/* GRE (4 bytes: flags=0, proto=IPv4) */
	p[0] = 0x00; p[1] = 0x00;
	p[2] = 0x08; p[3] = 0x00;
	p += 4;

	/* Inner IPv4 */
	auto *inner = reinterpret_cast<struct iphdr *>(p);
	inner->ihl      = 5;
	inner->version  = 4;
	inner->tos      = 0;
	inner->tot_len  = htons((uint16_t)inner_ip_len);
	inner->id       = 0;
	inner->frag_off = htons(IP_DF);
	inner->ttl      = 32;
	inner->protocol = IPPROTO_UDP;
	inner->saddr    = htonl(src.ip);
	inner->daddr    = mcast_ip_nbo;
	inner->check    = ip_csum(inner, sizeof(struct iphdr));
	p += sizeof(struct iphdr);

	/* Inner UDP */
	auto *udp   = reinterpret_cast<struct udphdr *>(p);
	udp->source = htons(60000);
	udp->dest   = udp_dst_port;
	udp->len    = htons((uint16_t)inner_udp_len);
	udp->check  = 0;
	/* payload bytes [0..15] left as zero (seq=0, ts_ns=0 placeholders) */

	return total;
}

/* ── usage ────────────────────────────────────────────────────────────── */
static void usage(const char *prog)
{
	printf("Usage: %s [options]\n"
	       "  -I <iface>       real NIC interface        (default: %s)\n"
	       "  -D <feeder-ip>   outer GRE dst (feeder)    (REQUIRED)\n"
	       "  -g <group>       inner multicast group     (default: %s)\n"
	       "  -p <port>        inner UDP dst port        (default: %d)\n"
	       "  -c <count>       number of packets         (default: %d)\n"
	       "  -i <interval_us> inter-packet gap µs       (default: %d)\n"
	       "  -s <size>        payload bytes             (default: %d, min: %d)\n"
	       "  -h               this help\n",
	       prog, DEF_IFACE, DEF_GROUP, DEF_PORT,
	       DEF_COUNT, DEF_INTERVAL_US, DEF_SIZE, HDR_SIZE);
}

int main(int argc, char *argv[])
{
	const char *iface       = DEF_IFACE;
	const char *feeder_ip_s = nullptr;
	const char *group       = DEF_GROUP;
	int port        = DEF_PORT;
	int count       = DEF_COUNT;
	int interval_us = DEF_INTERVAL_US;
	int pkt_size    = DEF_SIZE;

	int opt;
	while ((opt = getopt(argc, argv, "I:D:g:p:c:i:s:h")) != -1) {
		switch (opt) {
		case 'I': iface       = optarg;          break;
		case 'D': feeder_ip_s = optarg;          break;
		case 'g': group       = optarg;          break;
		case 'p': port        = atoi(optarg);    break;
		case 'c': count       = atoi(optarg);    break;
		case 'i': interval_us = atoi(optarg);    break;
		case 's': pkt_size    = atoi(optarg);    break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	if (!feeder_ip_s) {
		fprintf(stderr, "error: -D <feeder-ip> is required\n");
		usage(argv[0]);
		return 1;
	}
	if (pkt_size < HDR_SIZE) pkt_size = HDR_SIZE;

	/* ── interface info ───────────────────────────────────────────────── */
	iface_info src;
	if (!get_iface_info(iface, src)) return 1;

	/* ── resolve feeder MAC ───────────────────────────────────────────── */
	uint8_t dst_mac[6];
	printf("Resolving MAC for %s ...\n", feeder_ip_s);
	if (!resolve_mac(feeder_ip_s, iface, dst_mac)) {
		fprintf(stderr, "error: ARP resolution failed for %s\n", feeder_ip_s);
		fprintf(stderr, "       ensure feeder is reachable and try again\n");
		return 1;
	}
	printf("  feeder MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       dst_mac[0], dst_mac[1], dst_mac[2],
	       dst_mac[3], dst_mac[4], dst_mac[5]);

	/* ── network-order IPs ────────────────────────────────────────────── */
	uint32_t feeder_ip_nbo, mcast_ip_nbo;
	inet_pton(AF_INET, feeder_ip_s, &feeder_ip_nbo);
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
	int pkt_len = build_gre_pkt(
		(uint8_t *)umem_buf, UMEM_FRAME_SIZE,
		src, dst_mac, feeder_ip_nbo, mcast_ip_nbo,
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
	err = xsk_socket__create(&xsk, iface, 0, umem, nullptr, &tx_ring, &xsk_cfg);
	if (err) {
		fprintf(stderr, "native XDP failed (%s), trying SKB mode\n", strerror(-err));
		xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
		err = xsk_socket__create(&xsk, iface, 0, umem, nullptr, &tx_ring, &xsk_cfg);
	}
	if (err) { fprintf(stderr, "xsk_socket__create: %s\n", strerror(-err)); return 1; }
	int xsk_fd = xsk_socket__fd(xsk);

	printf("Sending %d packets to feeder %s (inner %s:%d) via %s  "
	       "payload=%dB  interval=%dus\n\n",
	       count, feeder_ip_s, group, port, iface, pkt_size, interval_us);

	uint64_t interval_ns = (uint64_t)interval_us * 1000ULL;
	uint32_t outstanding = 0;

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

		if (seq % 100 == 0) {
			printf("  sent %d/%d\r", seq, count);
			fflush(stdout);
		}

		if (interval_us > 0) {
			if (interval_us < 1000)
				busy_wait_ns(interval_ns);
			else {
				struct timespec sl{};
				sl.tv_sec  = interval_us / 1000000;
				sl.tv_nsec = (long)(interval_us % 1000000) * 1000L;
				nanosleep(&sl, nullptr);
			}
		}
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

	printf("\nDone. Sent %d packets.\n", count);
	xsk_socket__delete(xsk);
	xsk_umem__delete(umem);
	free(umem_buf);
	return 0;
}
