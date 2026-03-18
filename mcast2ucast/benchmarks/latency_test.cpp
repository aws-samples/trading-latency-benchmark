// latency_test.cpp — Roundtrip UDP latency measurement tool.
// Three modes:
//   1. "direct"   — Plain unicast UDP sockets (kernel baseline)
//   2. "mcast"    — Multicast via mcast0 TAP (mcast2ucast e2e)
//   3. "server"   — Reflect packets back (run on remote host)
//
// Build: g++ -O2 -std=c++17 -o latency_test latency_test.cpp -lpthread
//
// Usage:
//   Server (Instance 2):
//     ./latency_test server <listen-port>
//
//   Client direct baseline (Instance 1):
//     ./latency_test direct <server-ip> <server-port> [iterations] [payload-size]
//
//   Client mcast2ucast e2e (Instance 1):
//     ./latency_test mcast <mcast-group> <port> <mcast0-ip> <return-port> [iterations] [payload-size]
//
// Copyright 2024. MIT License.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>

static constexpr uint32_t MAGIC = 0x4C415400; // "LAT\0"
static constexpr int DEFAULT_ITERATIONS = 1000;
static constexpr int DEFAULT_PAYLOAD = 64;
static constexpr int TIMEOUT_MS = 500;

struct __attribute__((packed)) lat_pkt {
    uint32_t magic;
    uint32_t seq;
    uint64_t ts_ns;  // client send timestamp
    // followed by padding to reach payload size
};

static uint64_t now_ns() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

static void print_percentiles(std::vector<uint32_t>& latencies) {
    if (latencies.empty()) {
        printf("No successful packets\n");
        return;
    }
    std::sort(latencies.begin(), latencies.end());
    int n = (int)latencies.size();
    printf("Latency percentiles (usec):\n");
    for (int p = 0; p <= 99; p++) {
        if (p == 0 || p % 10 == 0 || p == 99) {
            int idx = (int)((long long)(n - 1) * p / 100);
            printf("  P%-2d: %.1f\n", p, latencies[idx] / 1000.0);
        }
    }
    // Also print P999
    int idx_999 = (int)((long long)(n - 1) * 999 / 1000);
    printf("  P99.9: %.1f\n", latencies[idx_999] / 1000.0);
}

// ---- Server mode: reflect packets back to sender ----
static int run_server(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return 1;
    }

    // Also join multicast groups if we receive multicast traffic
    // (for mcast mode, the server receives on the multicast group)
    printf("Server listening on port %d (reflects packets back)\n", port);
    printf("Press Ctrl+C to stop.\n");

    char buf[65536];
    while (true) {
        struct sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr*)&from, &fromlen);
        if (n < (ssize_t)sizeof(lat_pkt)) continue;

        auto* pkt = (lat_pkt*)buf;
        if (pkt->magic != MAGIC) continue;

        // Reflect back to sender
        sendto(fd, buf, n, 0, (struct sockaddr*)&from, fromlen);
    }
}

// ---- Server mode for multicast: join group, reflect via unicast ----
static int run_mcast_server(const char* group, int port, const char* iface_ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind to mcast0 if iface_ip provided
    if (iface_ip && strlen(iface_ip) > 0) {
        setsockopt(fd, SOL_SOCKET, 25, "mcast0", 7); // SO_BINDTODEVICE
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return 1;
    }

    // Join multicast group on mcast0 interface
    struct ip_mreq mreq{};
    inet_aton(group, &mreq.imr_multiaddr);
    if (iface_ip)
        inet_aton(iface_ip, &mreq.imr_interface);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("IP_ADD_MEMBERSHIP"); close(fd); return 1;
    }

    // Separate socket for replies — NOT bound to mcast0, so replies
    // go via normal kernel routing (primary NIC) instead of back through TAP.
    int reply_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (reply_fd < 0) { perror("reply socket"); close(fd); return 1; }

    printf("Multicast server listening on %s:%d (mcast0=%s)\n",
           group, port, iface_ip ? iface_ip : "any");
    printf("Press Ctrl+C to stop.\n");

    char buf[65536];
    while (true) {
        struct sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr*)&from, &fromlen);
        if (n < (ssize_t)sizeof(lat_pkt)) continue;

        auto* pkt = (lat_pkt*)buf;
        if (pkt->magic != MAGIC) continue;

        // Reflect back to sender via unicast using reply_fd (not bound to mcast0)
        sendto(reply_fd, buf, n, 0, (struct sockaddr*)&from, fromlen);
    }
}

// ---- Client: direct unicast ping-pong ----
static int run_direct(const char* server_ip, int server_port,
                      int iterations, int payload_size) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(server_port);
    inet_aton(server_ip, &dst.sin_addr);

    std::vector<char> buf(payload_size);
    std::vector<uint32_t> latencies;
    latencies.reserve(iterations);
    int timeouts = 0;

    printf("Direct unicast: %s:%d, %d iterations, %d byte payload\n",
           server_ip, server_port, iterations, payload_size);

    // Warmup
    for (int i = 0; i < 10; i++) {
        auto* pkt = (lat_pkt*)buf.data();
        pkt->magic = MAGIC;
        pkt->seq = 0;
        pkt->ts_ns = now_ns();
        sendto(fd, buf.data(), payload_size, 0,
               (struct sockaddr*)&dst, sizeof(dst));
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 100) > 0) {
            recv(fd, buf.data(), payload_size, 0);
        }
    }

    for (int i = 0; i < iterations; i++) {
        auto* pkt = (lat_pkt*)buf.data();
        pkt->magic = MAGIC;
        pkt->seq = i;
        pkt->ts_ns = now_ns();

        sendto(fd, buf.data(), payload_size, 0,
               (struct sockaddr*)&dst, sizeof(dst));

        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, TIMEOUT_MS) > 0) {
            ssize_t n = recv(fd, buf.data(), payload_size, 0);
            uint64_t end = now_ns();
            if (n >= (ssize_t)sizeof(lat_pkt) && pkt->magic == MAGIC) {
                uint32_t rtt_ns = (uint32_t)(end - pkt->ts_ns);
                latencies.push_back(rtt_ns);
            }
        } else {
            timeouts++;
        }
    }

    printf("\nResults:\n");
    printf("  Success: %zu/%d\n", latencies.size(), iterations);
    printf("  Timeouts: %d\n", timeouts);
    print_percentiles(latencies);

    close(fd);
    return 0;
}

// ---- Client: multicast via mcast0 (mcast2ucast e2e) ----
static int run_mcast(const char* mcast_group, int mcast_port,
                     const char* mcast0_ip, int return_port,
                     int iterations, int payload_size) {
    // Single socket: bind to mcast0_ip:return_port, send multicast, receive reply
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind to mcast0 IP and return_port so server replies come back here
    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(return_port);
    inet_aton(mcast0_ip, &bind_addr.sin_addr);
    if (bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind"); return 1;
    }

    // Set multicast interface to mcast0
    struct in_addr mcast_if{};
    inet_aton(mcast0_ip, &mcast_if);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mcast_if, sizeof(mcast_if));

    int ttl = 32;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(mcast_port);
    inet_aton(mcast_group, &dst.sin_addr);

    std::vector<char> buf(payload_size);
    std::vector<uint32_t> latencies;
    latencies.reserve(iterations);
    int timeouts = 0;

    printf("Multicast e2e: %s:%d via mcast0 (%s), return on port %d\n",
           mcast_group, mcast_port, mcast0_ip, return_port);
    printf("  %d iterations, %d byte payload\n", iterations, payload_size);
    printf("  Flow: app->TAP->mcast2ucast->wire->mcast2ucast->TAP->app->wire->app\n");

    // Warmup
    for (int i = 0; i < 10; i++) {
        auto* pkt = (lat_pkt*)buf.data();
        pkt->magic = MAGIC;
        pkt->seq = 0;
        pkt->ts_ns = now_ns();
        sendto(fd, buf.data(), payload_size, 0,
               (struct sockaddr*)&dst, sizeof(dst));
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 200) > 0) {
            recv(fd, buf.data(), payload_size, 0);
        }
    }

    for (int i = 0; i < iterations; i++) {
        auto* pkt = (lat_pkt*)buf.data();
        pkt->magic = MAGIC;
        pkt->seq = i;
        pkt->ts_ns = now_ns();

        sendto(fd, buf.data(), payload_size, 0,
               (struct sockaddr*)&dst, sizeof(dst));

        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, TIMEOUT_MS) > 0) {
            ssize_t n = recv(fd, buf.data(), payload_size, 0);
            uint64_t end = now_ns();
            if (n >= (ssize_t)sizeof(lat_pkt) && pkt->magic == MAGIC) {
                uint32_t rtt_ns = (uint32_t)(end - pkt->ts_ns);
                latencies.push_back(rtt_ns);
            }
        } else {
            timeouts++;
        }
    }

    printf("\nResults:\n");
    printf("  Success: %zu/%d\n", latencies.size(), iterations);
    printf("  Timeouts: %d\n", timeouts);
    print_percentiles(latencies);

    close(fd);
    return 0;
}

static void usage(const char* prog) {
    printf("Usage:\n");
    printf("  %s server <port>\n", prog);
    printf("  %s mcast-server <group> <port> <mcast0-ip>\n", prog);
    printf("  %s direct <server-ip> <port> [iterations] [payload-size]\n", prog);
    printf("  %s mcast <group> <port> <mcast0-ip> <return-port> [iterations] [payload-size]\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char* mode = argv[1];

    if (strcmp(mode, "server") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return run_server(atoi(argv[2]));
    }
    if (strcmp(mode, "mcast-server") == 0) {
        if (argc < 5) { usage(argv[0]); return 1; }
        return run_mcast_server(argv[2], atoi(argv[3]), argv[4]);
    }
    if (strcmp(mode, "direct") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        int iters = argc > 4 ? atoi(argv[4]) : DEFAULT_ITERATIONS;
        int psize = argc > 5 ? atoi(argv[5]) : DEFAULT_PAYLOAD;
        return run_direct(argv[2], atoi(argv[3]), iters, psize);
    }
    if (strcmp(mode, "mcast") == 0) {
        if (argc < 6) { usage(argv[0]); return 1; }
        int iters = argc > 6 ? atoi(argv[6]) : DEFAULT_ITERATIONS;
        int psize = argc > 7 ? atoi(argv[7]) : DEFAULT_PAYLOAD;
        return run_mcast(argv[2], atoi(argv[3]), argv[4],
                         atoi(argv[5]), iters, psize);
    }

    usage(argv[0]);
    return 1;
}
