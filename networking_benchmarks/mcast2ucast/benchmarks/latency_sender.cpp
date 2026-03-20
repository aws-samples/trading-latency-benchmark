#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>

static constexpr const char *DEF_IFACE = "mcast0";
static constexpr const char *DEF_GROUP = "224.0.0.101";
static constexpr int         DEF_PORT  = 5001;
static constexpr int         DEF_COUNT = 1000;
static constexpr int         DEF_INTERVAL_US = 1000;
static constexpr int         DEF_SIZE  = 64;
static constexpr int         HDR_SIZE  = 16; // seq(8) + ts_ns(8)

struct __attribute__((packed)) pkt_hdr {
	uint64_t seq;
	uint64_t ts_ns;
};

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
	uint64_t target;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	target = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec + ns;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
		if (now >= target)
			break;
	}
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n"
	       "  -I <iface>       interface (default: %s)\n"
	       "  -g <group>       multicast group (default: %s)\n"
	       "  -p <port>        destination port (default: %d)\n"
	       "  -c <count>       number of packets (default: %d)\n"
	       "  -i <interval_us> interval in microseconds (default: %d)\n"
	       "  -s <size>        payload size in bytes (default: %d, min: %d)\n"
	       "  -h               this help\n",
	       prog, DEF_IFACE, DEF_GROUP, DEF_PORT, DEF_COUNT, DEF_INTERVAL_US,
	       DEF_SIZE, HDR_SIZE);
}

int main(int argc, char *argv[])
{
	const char *iface = DEF_IFACE;
	const char *group = DEF_GROUP;
	int port          = DEF_PORT;
	int count         = DEF_COUNT;
	int interval_us   = DEF_INTERVAL_US;
	int pkt_size      = DEF_SIZE;

	int opt;
	while ((opt = getopt(argc, argv, "I:g:p:c:i:s:h")) != -1) {
		switch (opt) {
		case 'I': iface       = optarg;          break;
		case 'g': group       = optarg;          break;
		case 'p': port        = atoi(optarg);    break;
		case 'c': count       = atoi(optarg);    break;
		case 'i': interval_us = atoi(optarg);    break;
		case 's': pkt_size    = atoi(optarg);    break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	if (pkt_size < HDR_SIZE)
		pkt_size = HDR_SIZE;

	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	unsigned char ttl = 32;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

	if (strcmp(iface, "none") != 0) {
		if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface,
			       strlen(iface) + 1) < 0) {
			perror("SO_BINDTODEVICE");
			close(fd);
			return 1;
		}
	}

	struct sockaddr_in dst{};
	dst.sin_family = AF_INET;
	dst.sin_port   = htons(port);
	inet_pton(AF_INET, group, &dst.sin_addr);

	auto *buf = new uint8_t[pkt_size];
	memset(buf, 0, pkt_size);

	uint64_t interval_ns = (uint64_t)interval_us * 1000ULL;

	printf("Sending %d packets to %s:%d via %s  payload=%dB interval=%dus\n\n",
	       count, group, port, iface, pkt_size, interval_us);

	for (int seq = 0; seq < count; seq++) {
		auto *hdr = reinterpret_cast<pkt_hdr *>(buf);
		hdr->seq   = htobe64_((uint64_t)seq);
		hdr->ts_ns = htobe64_(now_ns());

		ssize_t n = sendto(fd, buf, pkt_size, 0,
				   (struct sockaddr *)&dst, sizeof(dst));
		if (n < 0) {
			perror("sendto");
			break;
		}

		if (seq % 100 == 0)
			printf("  sent %d/%d\r", seq, count);

		if (interval_us < 1000)
			busy_wait_ns(interval_ns);
		else {
			struct timespec sl = {
				.tv_sec  = interval_us / 1000000,
				.tv_nsec = (long)(interval_us % 1000000) * 1000L
			};
			nanosleep(&sl, nullptr);
		}
	}

	printf("\nDone. Sent %d packets.\n", count);
	delete[] buf;
	close(fd);
	return 0;
}
