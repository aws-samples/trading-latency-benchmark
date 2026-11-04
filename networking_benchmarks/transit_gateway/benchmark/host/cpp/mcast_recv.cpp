// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: MIT-0
//
// mcast_recv - multicast receiver for the TGW benchmark (Linux only).
//
// Joins a multicast group and measures one-way latency per packet from the
// sender's CLOCK_REALTIME stamp in the payload (see mcast_send.cpp).
//
//   primary   : NIC hardware receive stamp (ENA PHC, raw), enabled with
//               SIOCSHWTSTAMP. Required: the run fails if it is unavailable.
//   secondary : kernel software receive stamp (CLOCK_REALTIME, taken in the
//               NAPI path). Both arrive in the same SCM_TIMESTAMPING message.
//
// The software stamp is kept for divergence statistics: per packet,
// div = software - hardware. That is receive-path delay plus any offset between
// the system clock and the PHC, so it is a per-run check that the two clock
// domains agree, and it separates receiver-side stalls (in software only) from
// sender/network effects (in both).
//
// Usage:   mcast_recv <group> <port> <timeout_secs> [iface]
// stdout:  exactly ONE flat JSON line (the orchestrator captures stdout+stderr
//          into one file and save_report.py reads the first line). Diagnostics
//          for humans go nowhere on the success path.
// Files:   /tmp/mcast_recv.ready          created once bound, joined and stamping
//          /tmp/mcast_recv_detail.json    histograms, outliers, per-second maxima
//
// Exit codes: 0 = measured, 1 = failed (error JSON printed), 2 = usage.

#ifndef __linux__
#error "mcast_recv.cpp requires Linux (SO_TIMESTAMPING, recvmmsg, SIOCSHWTSTAMP)"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "lat_stats.hpp"

#ifndef SO_TIMESTAMPING_NEW
#define SO_TIMESTAMPING_NEW 65
#endif
#ifndef SCM_TIMESTAMPING_NEW
#define SCM_TIMESTAMPING_NEW 65
#endif
#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif

// Test builds only (-DMCAST_RECV_SYNTH_HW): environments such as loopback and veth
// have no hardware stamps. The test build tolerates a failed SIOCSHWTSTAMP and
// derives hw = sw - kSynthHwOffsetNs so the rest of the pipeline can be exercised.
// The shipped binary is built without it and always requires a real hardware stamp.
constexpr int64_t kSynthHwOffsetNs = 700;
#ifdef MCAST_RECV_SYNTH_HW
constexpr bool kSynthHw = true;
#else
constexpr bool kSynthHw = false;
#endif

using latstats::JsonLine;
using latstats::LatencyRecorder;
using latstats::Sample;

namespace {

constexpr uint32_t kEndSeq = 0xFFFFFFFFu;
constexpr size_t kPayloadLen = 12;
constexpr int kBatch = 64;
constexpr size_t kPktBuf = 2048;
constexpr size_t kCtrlBuf = 256;
constexpr int kPostEnableSettleMs = 500;   // let the NIC finish reconfiguring
// A sender that is already running can deliver a packet before the kernel's stamping
// is effective for this socket. Such packets are skipped (and counted) until the first
// fully stamped packet arrives; after that a missing stamp is a hard failure.
constexpr uint64_t kMaxUnstampedStartup = 5000;
constexpr int kPostEndGraceMs = 250;       // collect stragglers after the end marker
const char* const kReadyPath = "/tmp/mcast_recv.ready";
const char* const kDetailPath = "/tmp/mcast_recv_detail.json";

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

inline int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
inline uint32_t get_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline uint64_t get_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(get_be32(p)) << 32) | get_be32(p + 4);
}

// SCM_TIMESTAMPING payload: three timespecs. [0] software, [1] deprecated,
// [2] raw hardware.
struct Stamps {
    int64_t sw_ns = 0, hw_ns = 0;
    bool has_sw = false, has_hw = false;
    bool has_drops = false;
    uint32_t rxq_drops = 0;
};

Stamps parse_cmsgs(msghdr* mh) {
    Stamps s;
    for (cmsghdr* c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c)) {
        if (c->cmsg_level != SOL_SOCKET) continue;
        if (c->cmsg_type == SCM_TIMESTAMPING_NEW || c->cmsg_type == SCM_TIMESTAMPING) {
            // Layout is struct timespec[3] (old) or __kernel_timespec[3] (new);
            // both are 3 x {int64, int64} on 64-bit targets.
            if (c->cmsg_len < CMSG_LEN(6 * sizeof(int64_t))) continue;
            int64_t v[6];
            memcpy(v, CMSG_DATA(c), sizeof v);
            if (v[0] != 0 || v[1] != 0) {
                s.sw_ns = v[0] * 1000000000LL + v[1];
                s.has_sw = true;
            }
            if (v[4] != 0 || v[5] != 0) {
                s.hw_ns = v[4] * 1000000000LL + v[5];
                s.has_hw = true;
            }
        } else if (c->cmsg_type == SO_RXQ_OVFL && c->cmsg_len >= CMSG_LEN(sizeof(uint32_t))) {
            memcpy(&s.rxq_drops, CMSG_DATA(c), sizeof(uint32_t));
            s.has_drops = true;
        }
    }
    return s;
}

std::string detect_iface() {
    std::ifstream f("/proc/net/route");
    std::string line, best;
    long best_metric = -1;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string iface, dest, gw, flags, refcnt, use, metric, mask;
        is >> iface >> dest >> gw >> flags >> refcnt >> use >> metric >> mask;
        if (dest == "00000000" && mask == "00000000") {
            const long m = strtol(metric.c_str(), nullptr, 10);
            if (best_metric < 0 || m < best_metric) {
                best_metric = m;
                best = iface;
            }
        }
    }
    if (!best.empty()) return best;
    return "eth0";
}

std::map<std::string, long long> read_udp_snmp() {
    std::map<std::string, long long> out;
    std::ifstream f("/proc/net/snmp");
    std::string l1, l2;
    while (std::getline(f, l1)) {
        if (l1.rfind("Udp:", 0) != 0) continue;
        if (!std::getline(f, l2)) break;
        std::istringstream a(l1), b(l2);
        std::string ka, kb;
        a >> ka;
        b >> kb;
        std::string name;
        long long val;
        while (a >> name && b >> val) out[name] = val;
        break;
    }
    return out;
}

int set_hw_timestamping(int fd, const std::string& iface, int rx_filter, int* applied) {
    hwtstamp_config cfg{};
    cfg.tx_type = HWTSTAMP_TX_OFF;
    cfg.rx_filter = rx_filter;
    ifreq ifr{};
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(&cfg);
    if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) return errno;
    if (applied) *applied = cfg.rx_filter;
    return 0;
}

void write_atomic(const char* path, const std::string& body) {
    const std::string tmp = std::string(path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << body << "\n";
    }
    rename(tmp.c_str(), path);
}

// Anything that must be undone on every exit path.
struct Cleanup {
    int fd = -1;
    std::string iface;
    bool hw_enabled = false;
    ~Cleanup() {
        unlink(kReadyPath);
        if (hw_enabled && fd >= 0) set_hw_timestamping(fd, iface, HWTSTAMP_FILTER_NONE, nullptr);
        if (fd >= 0) close(fd);
    }
};

int fail(const std::string& err, const std::string& detail, const std::string& iface,
         const std::string& hint = "") {
    JsonLine j;
    j.str("error", err).str("detail", detail).str("iface", iface).str("impl", "cpp");
    if (!hint.empty()) j.str("hint", hint);
    printf("%s\n", j.done().c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s <group> <port> <timeout_secs> [iface]\n", argv[0]);
        return 2;
    }
    const std::string group = argv[1];
    char* e1 = nullptr;
    char* e2 = nullptr;
    const long port = strtol(argv[2], &e1, 10);
    const long timeout_s = strtol(argv[3], &e2, 10);
    in_addr gaddr{};
    if (*e1 || *e2 || port < 1 || port > 65535 || timeout_s < 1 || timeout_s > 86400 ||
        inet_pton(AF_INET, group.c_str(), &gaddr) != 1) {
        fprintf(stderr, "usage: %s <group> <port> <timeout_secs> [iface]\n", argv[0]);
        return 2;
    }
    const std::string iface = argc == 5 ? argv[4] : detect_iface();
    unlink(kReadyPath);

    if (sysconf(_SC_NPROCESSORS_ONLN) > 1) {  // pin to the second CPU
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(1, &set);
        sched_setaffinity(0, sizeof set, &set);
    }

    Cleanup guard;
    guard.iface = iface;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return fail("socket failed", strerror(errno), iface);
    guard.fd = fd;

    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    const int rcvbuf = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    int rcvbuf_effective = 0;   // the kernel clamps the request to 2 x net.core.rmem_max
    {
        socklen_t l = sizeof rcvbuf_effective;
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_effective, &l);
    }
    const int busy = 50;
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy, sizeof busy);   // best effort
    setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof one);      // per-packet drop counter

    const uint32_t ts_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
                              SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    const char* variant = "SO_TIMESTAMPING_NEW";
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING_NEW, &ts_flags, sizeof ts_flags) < 0) {
        variant = "SO_TIMESTAMPING";
        if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags, sizeof ts_flags) < 0)
            return fail("SO_TIMESTAMPING failed", strerror(errno), iface);
    }

    // Enable hardware receive stamping. Per the ENA README this momentarily
    // disrupts networking, so it happens before the group join and before
    // readiness is signalled.
    int hw_filter_applied = -1;
    const int64_t t_ioctl0 = mono_ns();
    const int hw_err = set_hw_timestamping(fd, iface, HWTSTAMP_FILTER_ALL, &hw_filter_applied);
    const double ioctl_ms = static_cast<double>(mono_ns() - t_ioctl0) / 1e6;
    if (hw_err == 0) {
        guard.hw_enabled = true;
    } else if (!kSynthHw) {
        return fail("SIOCSHWTSTAMP failed", strerror(hw_err), iface,
                    "Instance must be PHC-capable (m7/c7/r7/i7/i8/c8/m8/r8/x8) with ena phc_enable=1, "
                    "and the run needs CAP_NET_ADMIN. Check `ethtool -T <iface>`.");
    }
    if (hw_err == 0 && kPostEnableSettleMs > 0) {
        const timespec ts{0, kPostEnableSettleMs * 1000000L};
        nanosleep(&ts, nullptr);
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof local) < 0)
        return fail("bind failed", strerror(errno), iface);
    ip_mreq mreq{};
    mreq.imr_multiaddr = gaddr;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    // A non-multicast "group" (used for unicast loopback tests) needs no join.
    if (IN_MULTICAST(ntohl(gaddr.s_addr)) &&
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0)
        return fail("IP_ADD_MEMBERSHIP failed", strerror(errno), iface);

    struct sigaction sa{};
    sa.sa_handler = on_signal;   // no SA_RESTART: interrupt poll()
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const auto snmp0 = read_udp_snmp();

    // Ready: socket bound and joined, hardware stamping on, signals installed.
    {
        char pid[32];
        snprintf(pid, sizeof pid, "%d", static_cast<int>(getpid()));
        write_atomic(kReadyPath, pid);
    }

    static uint8_t bufs[kBatch][kPktBuf];
    alignas(8) static uint8_t ctrl[kBatch][kCtrlBuf];
    iovec iov[kBatch];
    mmsghdr msgs[kBatch];

    LatencyRecorder rec;
    {   // Latency of the first seconds is discarded (loss is still counted); 0 disables.
        const char* w = getenv("MCAST_WARMUP_SECS");
        const double warm_s = w ? atof(w) : 2.0;
        if (warm_s > 0) rec.set_warmup_ns(static_cast<int64_t>(warm_s * 1e9));
    }
    uint64_t raw_data_packets = 0, hw_count = 0, short_packets = 0, end_markers = 0;
    uint32_t max_rxq_drops = 0;
    bool end_seen = false, timed_out = false, error = false;
    bool stamping_confirmed = false;
    uint64_t startup_unstamped = 0;
    uint64_t sender_total = 0;
    int64_t max_seq_seen = -1;
    std::string err_msg;
    int64_t end_deadline = 0;
    const int64_t t_start = mono_ns();

    pollfd pfd{fd, POLLIN, 0};
    while (!g_stop && !error) {
        int wait_ms = static_cast<int>(timeout_s * 1000);
        if (end_seen) {
            const int64_t left = (end_deadline - mono_ns()) / 1000000;
            if (left <= 0) break;
            wait_ms = static_cast<int>(left);
        }
        const int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            err_msg = std::string("poll: ") + strerror(errno);
            error = true;
            break;
        }
        if (pr == 0) {
            if (!end_seen) timed_out = true;
            break;
        }

        for (;;) {   // drain
            for (int i = 0; i < kBatch; ++i) {
                iov[i] = {bufs[i], kPktBuf};
                msgs[i].msg_hdr = {};
                msgs[i].msg_hdr.msg_iov = &iov[i];
                msgs[i].msg_hdr.msg_iovlen = 1;
                msgs[i].msg_hdr.msg_control = ctrl[i];
                msgs[i].msg_hdr.msg_controllen = kCtrlBuf;
                msgs[i].msg_len = 0;
            }
            const int n = recvmmsg(fd, msgs, kBatch, MSG_DONTWAIT, nullptr);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                err_msg = std::string("recvmmsg: ") + strerror(errno);
                error = true;
                break;
            }
            for (int i = 0; i < n && !error; ++i) {
                Stamps st = parse_cmsgs(&msgs[i].msg_hdr);
                if (st.has_drops && st.rxq_drops > max_rxq_drops) max_rxq_drops = st.rxq_drops;
                if (!st.has_hw) {
                    if (kSynthHw && st.has_sw) {
                        st.hw_ns = st.sw_ns - kSynthHwOffsetNs;
                        st.has_hw = true;
                    } else if (!stamping_confirmed && ++startup_unstamped <= kMaxUnstampedStartup) {
                        continue;   // startup race, see kMaxUnstampedStartup
                    } else {
                        err_msg = "packet without a hardware receive timestamp";
                        error = true;
                        break;
                    }
                } else {
                    ++hw_count;
                }
                if (st.has_hw && (kSynthHw || st.has_sw)) stamping_confirmed = true;
                const size_t len = msgs[i].msg_len;
                if (len < kPayloadLen) {
                    ++short_packets;
                    continue;
                }
                const uint32_t seq = get_be32(bufs[i]);
                const uint64_t tx = get_be64(bufs[i] + 4);
                if (seq == kEndSeq) {
                    ++end_markers;
                    if (!end_seen) {
                        end_seen = true;
                        sender_total = tx;
                        end_deadline = mono_ns() + static_cast<int64_t>(kPostEndGraceMs) * 1000000LL;
                    }
                    continue;
                }
                ++raw_data_packets;
                if (static_cast<int64_t>(seq) > max_seq_seen) max_seq_seen = static_cast<int64_t>(seq);
                Sample s;
                s.seq = seq;
                s.tx_ns = static_cast<int64_t>(tx);
                s.hw_ns = st.hw_ns;
                s.sw_ns = st.sw_ns;
                s.has_sw = st.has_sw;
                rec.record(s);
            }
            if (error || n < kBatch) break;
        }
    }
    const bool interrupted = g_stop != 0;
    const double run_s = static_cast<double>(mono_ns() - t_start) / 1e9;
    const auto snmp1 = read_udp_snmp();

    if (error) {
        const int rc = fail(err_msg == "packet without a hardware receive timestamp"
                                ? "RAW_HARDWARE timestamp is zero"
                                : err_msg,
                            err_msg, iface,
                            "Verify /sys/module/ena/parameters/phc_enable == 1, `ethtool -T <iface>` shows "
                            "hardware-raw-clock, and /dev/ptp_ena exists.");
        return rc;
    }

    // Accounting
    const uint64_t expected = end_seen ? sender_total : (max_seq_seen >= 0 ? static_cast<uint64_t>(max_seq_seen) + 1 : 0);
    const uint64_t unique = rec.seq().unique();
    const uint64_t loss = expected > unique ? expected - unique : 0;
    auto delta = [&](const char* k) -> long long {
        const auto a = snmp0.find(k), b = snmp1.find(k);
        return (a != snmp0.end() && b != snmp1.end()) ? b->second - a->second : -1;
    };

    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);

    JsonLine j;
    // Fields the existing report scripts already read.
    j.str("iface", iface).str("so_timestamping_variant", variant);
    j.boolean("hw_timestamping_enabled", hw_err == 0);
    j.u64("total_expected", expected).u64("total_received", unique).u64("packet_loss_count", loss);
    j.u64("hw_timestamp_count", hw_count).boolean("end_marker_received", end_seen);
    rec.append_summary(j);   // min/max/mean/median/p95/p99 (+p999, sw_*, div_*, accounting)
    // Additional run context.
    j.str("impl", "cpp").boolean("synthetic_hw", kSynthHw);
    j.u64("packets_received_raw", raw_data_packets).u64("short_packets", short_packets);
    j.u64("startup_unstamped_skipped", startup_unstamped).u64("end_markers_seen", end_markers).boolean("received_exceeds_expected", unique > expected);
    j.boolean("timed_out", timed_out).boolean("interrupted", interrupted);
    j.i64("rcvbuf_bytes_effective", rcvbuf_effective).u64("socket_drops", max_rxq_drops);
    j.i64("udp_rcvbuf_errors_delta", delta("RcvbufErrors")).i64("udp_in_errors_delta", delta("InErrors"));
    j.i64("hw_rx_filter_applied", hw_filter_applied).f3("hw_enable_ioctl_ms", ioctl_ms);
    j.f3("run_s", run_s);
    j.f3("cpu_user_s", ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6);
    j.f3("cpu_sys_s", ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);

    write_atomic(kDetailPath, rec.detail_json());
    j.str("detail_file", kDetailPath);
    printf("%s\n", j.done().c_str());
    fflush(stdout);
    return 0;
}
