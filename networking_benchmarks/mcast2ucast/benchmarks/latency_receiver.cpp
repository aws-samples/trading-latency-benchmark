#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <algorithm>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <cerrno>

static constexpr int DEF_PORT    = 5001;
static constexpr int DEF_COUNT   = 1000;
static constexpr int DEF_TIMEOUT = 30;
static constexpr int HDR_SIZE    = 16;

struct __attribute__((packed)) pkt_hdr {
	uint64_t seq;
	uint64_t ts_ns;
};

static volatile sig_atomic_t interrupted = 0;

static void sig_handler(int) { interrupted = 1; }

static inline uint64_t betoh64_(uint64_t v)
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

static uint64_t pct(std::vector<uint64_t> &sorted, int p)
{
	if (sorted.empty()) return 0;
	size_t idx = (size_t)((sorted.size() - 1) * p) / 100;
	return sorted[idx];
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n"
	       "  -p <port>     UDP port (default: %d)\n"
	       "  -c <count>    packets to receive (default: %d)\n"
	       "  -t <timeout>  seconds to wait (default: %d)\n"
	       "  -r            print raw latencies\n"
	       "  -h            this help\n",
	       prog, DEF_PORT, DEF_COUNT, DEF_TIMEOUT);
}

int main(int argc, char *argv[])
{
	int port    = DEF_PORT;
	int count   = DEF_COUNT;
	int timeout = DEF_TIMEOUT;
	bool raw    = false;

	int opt;
	while ((opt = getopt(argc, argv, "p:c:t:rh")) != -1) {
		switch (opt) {
		case 'p': port    = atoi(optarg); break;
		case 'c': count   = atoi(optarg); break;
		case 't': timeout = atoi(optarg); break;
		case 'r': raw     = true;         break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	signal(SIGINT, sig_handler);

	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) { perror("socket"); return 1; }

	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return 1;
	}

	struct timeval tv = { .tv_sec = timeout, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	std::vector<uint64_t> latencies;
	latencies.reserve(count);

	uint8_t buf[65536];
	int received  = 0;
	int lost      = 0;
	int ooo       = 0;
	int64_t last_seq = -1;
	uint64_t min_lat = UINT64_MAX;
	uint64_t max_lat = 0;
	uint64_t sum_lat = 0;

	printf("Listening on UDP port %d, expecting %d packets (timeout=%ds)\n\n",
	       port, count, timeout);

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (received < count && !interrupted) {
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				printf("Timeout after %ds\n", timeout);
				break;
			}
			break;
		}

		uint64_t rx_ns = now_ns();

		if (n < HDR_SIZE) continue;

		auto *hdr = reinterpret_cast<pkt_hdr *>(buf);
		uint64_t seq   = betoh64_(hdr->seq);
		uint64_t tx_ns = betoh64_(hdr->ts_ns);

		int64_t lat = (int64_t)(rx_ns - tx_ns);
		if (lat < 0) lat = 0;

		uint64_t ulat = (uint64_t)lat;
		latencies.push_back(ulat);
		sum_lat += ulat;
		received++;

		if (ulat < min_lat) min_lat = ulat;
		if (ulat > max_lat) max_lat = ulat;

		/* Track ordering */
		int64_t iseq = (int64_t)seq;
		if (last_seq >= 0) {
			if (iseq < last_seq)
				ooo++;
			else if (iseq > last_seq + 1)
				lost += (int)(iseq - last_seq - 1);
		}
		if (iseq > last_seq)
			last_seq = iseq;

		if (received % 100 == 0) {
			/* Rolling average of last 100 */
			uint64_t avg100 = 0;
			int start = (int)latencies.size() - 100;
			if (start < 0) start = 0;
			for (size_t i = start; i < latencies.size(); i++)
				avg100 += latencies[i];
			avg100 /= (latencies.size() - start);
			printf("  [%d/%d] last=%.1fus avg(100)=%.1fus\r",
			       received, count,
			       ulat / 1000.0, avg100 / 1000.0);
			fflush(stdout);
		}
	}

	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed = (t1.tv_sec - t0.tv_sec) +
			 (t1.tv_nsec - t0.tv_nsec) / 1e9;

	close(fd);

	if (received == 0) {
		printf("No packets received.\n");
		return 1;
	}

	std::sort(latencies.begin(), latencies.end());
	uint64_t avg_lat = sum_lat / received;

	printf("\n\n");
	printf("==================================================\n");
	printf("  mcast2ucast Latency Report\n");
	printf("==================================================\n");
	printf("  Received:      %d/%d packets (%.1fs)\n", received, count, elapsed);
	printf("  Lost:          %d packets\n", lost);
	printf("  Out of order:  %d\n", ooo);
	printf("\n");
	printf("  Latency (usec):\n");
	printf("    Min:    %10.1f\n", min_lat / 1000.0);
	printf("    Avg:    %10.1f\n", avg_lat / 1000.0);
	printf("    Max:    %10.1f\n", max_lat / 1000.0);
	printf("\n");
	printf("  Percentiles (usec):\n");
	static const int pcts[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 99};
	for (int p : pcts)
		printf("    P%-3d  %10.1f\n", p, pct(latencies, p) / 1000.0);
	printf("==================================================\n");

	if (raw) {
		printf("\nRaw latencies (ns):\n");
		for (size_t i = 0; i < latencies.size(); i++)
			printf("  %zu: %" PRIu64 "\n", i, latencies[i]);
	}

	return 0;
}
