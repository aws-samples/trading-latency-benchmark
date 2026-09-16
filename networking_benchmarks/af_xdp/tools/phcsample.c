// phcsample - sample the ENA PHC against CLOCK_REALTIME and record the
// device-reported error bound alongside chrony's own error terms.
//
// Emits one CSV row per sample:
//   seq,wall_ns,phc_ns,sysmid_ns,offset_ns,bracket_ns,eb_ns
//
// offset_ns  = phc - midpoint(sys_before, sys_after)
// bracket_ns = sys_after - sys_before  (the measurement's own noise floor)
// eb_ns      = phc_error_bound read from sysfs at the same instant
//
// Each row is the tightest-bracket sample out of NS_PER_IOCTL taken in one
// PTP_SYS_OFFSET_EXTENDED ioctl. NS_PER_IOCTL is NOT free: the kernel's
// ptp_sys_offset_extended() loops n_samples times, calling the driver's
// gettimex64 on each iteration - so ENA's ena_com_phc_get_timestamp() (one
// doorbell write + device round trip) runs once PER SAMPLE, not once per
// ioctl. One call with n_per=5 costs 5 requests against ENA's 125 req/sec
// device cap, not 1. At hz=10, n_per=5 that is 50 requests/sec - verified
// against ena_com.c: ena_com_phc_get_timestamp() is called once per iteration
// of the sample loop in the kernel's PTP_SYS_OFFSET_EXTENDED handler.
// A prior version of this comment claimed 1 request per ioctl; that was
// wrong and run-battery.sh's own budget comment (which says 50/sec) was
// right. Do not raise hz*n_per without re-deriving the request rate from
// this formula: requests/sec = hz * n_per_ioctl.
//
// Exceeding the cap does not just fail the excess requests: the device
// enters a block state (default ENA_PHC_DEFAULT_BLOCK_TIMEOUT_USEC = 1000us)
// during which EVERY get-time request - including chronyd's own polling -
// returns EBUSY. One collision can cost most of a sampling window (see the
// EBUSY handling below and FINDINGS.md's run1 destination sampler loss).
//
// Build: gcc -O2 -Wall -o phcsample phcsample.c
// Usage: phcsample <ptp-dev> <eb-sysfs-path> <hz> <seconds> [n_per_ioctl]

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/ptp_clock.h>

// Consecutive ioctl failures tolerated before concluding the device is genuinely
// unavailable rather than momentarily contended.
#define MAX_CONSEC_FAIL 20

static inline int64_t pct_to_ns(struct ptp_clock_time t) {
    return (int64_t)t.sec * 1000000000LL + (int64_t)t.nsec;
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Read phc_error_bound (an unsigned decimal in nanoseconds) from sysfs.
// Returns -1 on any failure so a missing attribute is visible in the data
// rather than silently recorded as zero.
static int64_t read_eb(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    errno = 0;
    long long v = strtoll(buf, NULL, 10);
    if (errno != 0) return -1;
    return (int64_t)v;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s <ptp-dev> <eb-sysfs-path> <hz> <seconds> [n_per_ioctl]\n",
                argv[0]);
        return 2;
    }
    const char *dev = argv[1];
    const char *ebpath = argv[2];
    double hz = atof(argv[3]);
    double secs = atof(argv[4]);
    unsigned n_per = (argc > 5) ? (unsigned)atoi(argv[5]) : 10u;

    if (hz <= 0 || secs <= 0) {
        fprintf(stderr, "hz and seconds must be > 0\n");
        return 2;
    }
    if (n_per < 1 || n_per > PTP_MAX_SAMPLES) {
        fprintf(stderr, "n_per_ioctl must be 1..%d\n", PTP_MAX_SAMPLES);
        return 2;
    }

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    long long total = (long long)(hz * secs);
    long long period_ns = (long long)(1e9 / hz);
    long long ioctl_fail = 0, consec_fail = 0;

    // Line-buffer stdout explicitly. glibc's default is FULLY buffered
    // whenever stdout is not a TTY - which it never is here, since the
    // control-plane agent connects it to a pipe (Go's exec.Cmd). At 10 Hz
    // that means rows silently accumulate in an unflushed buffer for
    // several seconds before glibc's own block-size threshold forces a
    // flush. A caller that stops this process early (StopWander's
    // SIGKILL - see runner.go) gives it no chance to flush on exit, so a
    // short-duration sampling window can lose ALL of its rows even though
    // they were genuinely sampled. Confirmed live: a 1.5s window at 10 Hz
    // (~15 expected rows) returned an entirely empty CSV to the caller.
    // Line buffering flushes after every '\n', so a killed process still
    // hands back every row it had already printed before the signal.
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("seq,wall_ns,phc_ns,sysmid_ns,offset_ns,bracket_ns,eb_ns\n");

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    for (long long i = 0; i < total; i++) {
        struct ptp_sys_offset_extended pso;
        memset(&pso, 0, sizeof(pso));
        pso.n_samples = n_per;

        if (ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &pso) != 0) {
            // A transient EBUSY must not end the run. chronyd polls the same
            // device, so the two of us occasionally collide and the driver returns
            // EBUSY; exiting on the first one cost 90% of a sampling window in
            // practice. Emit a sentinel row so the gap is visible in the data
            // rather than silently absent, and give up only if failures dominate.
            ioctl_fail++;
            consec_fail++;
            printf("%lld,%lld,-1,-1,-1,-1,-1\n", i, (long long)now_ns());
            if (consec_fail >= MAX_CONSEC_FAIL) {
                fprintf(stderr, "ioctl PTP_SYS_OFFSET_EXTENDED: %s "
                        "(%lld consecutive failures, giving up at seq %lld)\n",
                        strerror(errno), (long long)consec_fail, (long long)i);
                close(fd);
                return 1;
            }
            goto pace;
        }
        consec_fail = 0;
        int64_t eb = read_eb(ebpath);

        // Pick the sample with the tightest sys_before..sys_after bracket: it
        // is the one whose midpoint most precisely locates the PHC read.
        int64_t best_bracket = INT64_MAX, best_off = 0, best_phc = 0, best_mid = 0;
        for (unsigned s = 0; s < n_per; s++) {
            int64_t b = pct_to_ns(pso.ts[s][0]);
            int64_t p = pct_to_ns(pso.ts[s][1]);
            int64_t a = pct_to_ns(pso.ts[s][2]);
            int64_t br = a - b;
            if (br < best_bracket) {
                best_bracket = br;
                best_mid = b + br / 2;
                best_phc = p;
                best_off = p - best_mid;
            }
        }

        printf("%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
               i, (long long)now_ns(), (long long)best_phc, (long long)best_mid,
               (long long)best_off, (long long)best_bracket, (long long)eb);

    pace:
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    if (ioctl_fail) {
        fprintf(stderr, "phcsample: %lld of %lld samples failed the ioctl "
                "(%.2f%%), recorded as -1 rows\n",
                (long long)ioctl_fail, total, 100.0 * ioctl_fail / (double)total);
    }
    close(fd);
    return 0;
}
