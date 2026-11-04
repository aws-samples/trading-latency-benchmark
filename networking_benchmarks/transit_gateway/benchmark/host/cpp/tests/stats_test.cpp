// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: MIT-0
//
// Unit tests for lat_stats.hpp. Portable: builds and runs on macOS and Linux.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../lat_stats.hpp"

using namespace latstats;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_fail;                                                        \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

// Exact quantile with the rule used by the receiver and results.py: sorted[min(int(n*q), n-1)].
static int64_t exact_q(std::vector<int64_t> v, double q) {
    std::sort(v.begin(), v.end());
    size_t k = static_cast<size_t>(static_cast<double>(v.size()) * q);
    if (k >= v.size()) k = v.size() - 1;
    return v[k];
}

static void test_bucket_geometry() {
    std::mt19937_64 rng(1);
    for (int i = 0; i < 200000; ++i) {
        const uint64_t v = rng() >> (rng() % 40 + 20);  // wide dynamic range
        const size_t idx = LogHist::index(v);
        if (v >= (1ULL << LogHist::MAX_BITS)) continue;
        CHECK(idx < LogHist::NBUCKETS);
        const uint64_t lo = LogHist::lower(idx), w = LogHist::width(idx);
        CHECK(lo <= v && v < lo + w);
        if (v >= LogHist::SUB) CHECK(static_cast<double>(w) / static_cast<double>(lo) <= 1.0 / 1024 + 1e-12);
    }
    CHECK(LogHist::index(0) == 0);
    CHECK(LogHist::index(1023) == 1023);
    CHECK(LogHist::index(1ULL << 60) == LogHist::NBUCKETS - 1);  // saturates
}

static void test_quantiles_vs_exact() {
    struct Case { const char* name; std::vector<int64_t> v; };
    std::mt19937_64 rng(7);
    std::vector<Case> cases;

    {
        Case c{"lognormal ~170us", {}};
        std::lognormal_distribution<double> d(std::log(170000.0), 0.15);
        for (int i = 0; i < 300000; ++i) c.v.push_back(static_cast<int64_t>(d(rng)));
        cases.push_back(c);
    }
    {
        Case c{"heavy tail + 5ms spikes", {}};
        std::lognormal_distribution<double> d(std::log(160000.0), 0.1);
        for (int i = 0; i < 100000; ++i) c.v.push_back(static_cast<int64_t>(d(rng)) + (i % 20011 == 0 ? 5000000 : 0));
        cases.push_back(c);
    }
    {
        Case c{"signed (skewed clock)", {}};
        std::normal_distribution<double> d(-20000.0, 60000.0);
        for (int i = 0; i < 100000; ++i) c.v.push_back(static_cast<int64_t>(d(rng)));
        cases.push_back(c);
    }
    {
        Case c{"tiny values", {}};
        for (int i = 0; i < 5000; ++i) c.v.push_back(static_cast<int64_t>(rng() % 900));
        cases.push_back(c);
    }
    {
        Case c{"single sample", {123456}};
        cases.push_back(c);
    }

    for (const auto& c : cases) {
        SignedHist h;
        for (int64_t x : c.v) h.add(x);
        CHECK(h.count() == c.v.size());
        CHECK(h.min() == *std::min_element(c.v.begin(), c.v.end()));
        CHECK(h.max() == *std::max_element(c.v.begin(), c.v.end()));
        long double s = 0;
        for (int64_t x : c.v) s += x;
        CHECK(std::fabs(h.mean() - static_cast<double>(s / c.v.size())) < 1e-6 * (1 + std::fabs(h.mean())));
        for (double q : {0.01, 0.25, 0.50, 0.90, 0.95, 0.99, 0.999}) {
            const int64_t e = exact_q(c.v, q), a = h.quantile(q);
            // bucket midpoint error: <= 0.05% of the value, +1 ns for tiny values
            const double tol = std::fabs(static_cast<double>(e)) * 0.0006 + 1.0;
            if (std::fabs(static_cast<double>(a - e)) > tol)
                printf("  [%s] q=%.3f exact=%lld hist=%lld\n", c.name, q, (long long)e, (long long)a);
            CHECK(std::fabs(static_cast<double>(a - e)) <= tol);
        }
    }
}

static void test_seq_tracker() {
    SeqTracker t;
    for (uint32_t s : {0u, 1u, 2u, 4u, 3u, 5u}) t.observe(s);   // 3 arrives after 4: one out-of-order
    CHECK(t.unique() == 6 && t.out_of_order() == 1 && t.duplicates() == 0);
    CHECK(t.observe(2).duplicate);
    CHECK(t.observe(5).duplicate);
    CHECK(t.duplicates() == 2 && t.unique() == 6);
    CHECK(t.max_seq() == 5);
    // A duplicate must not count as out-of-order.
    CHECK(t.out_of_order() == 1);
    // Huge jumps are untracked, not allocated.
    SeqTracker g;
    g.observe(0);
    const auto r = g.observe(900000000u);
    CHECK(!r.tracked && g.untracked() == 1 && g.unique() == 1);
    // First packet with a large sequence number is also bounded.
    SeqTracker g2;
    CHECK(!g2.observe(700000000u).tracked);
    // Sequences beyond the cap are untracked.
    CHECK(!g.observe(0xFFFFFFF0u).tracked);
    // A long run, with gaps, is tracked exactly.
    SeqTracker big;
    uint64_t sent = 0;
    for (uint32_t s = 0; s < 3000000; ++s) if (s % 97 != 0) { big.observe(s); ++sent; }
    CHECK(big.unique() == sent && big.duplicates() == 0 && big.out_of_order() == 0);
}

static Sample mk(uint32_t seq, int64_t tx, int64_t hw_lat, int64_t div) {
    Sample s;
    s.seq = seq; s.tx_ns = tx; s.hw_ns = tx + hw_lat; s.sw_ns = s.hw_ns + div; s.has_sw = true;
    return s;
}

static void test_recorder() {
    LatencyRecorder r;
    const int64_t base = 1700000000000000000LL;
    for (uint32_t i = 0; i < 3000; ++i)  // 3 s at 1 kpps
        r.record(mk(i, base + i * 1000000LL, 150000 + (i % 7) * 1000, 800 + (i % 3) * 100));
    // injected faults
    r.record(mk(3000, base + 3000 * 1000000LL, 4900000, 900));       // 4.9 ms hardware outlier
    r.record(mk(3001, base + 3001 * 1000000LL, -25000, 900));        // negative latency: skewed clock
    r.record(mk(3001, base + 3001 * 1000000LL, 150000, 900));        // duplicate, must be ignored for latency
    Sample nosw = mk(3002, base + 3002 * 1000000LL, 160000, 0); nosw.has_sw = false;
    r.record(nosw);

    CHECK(r.samples() == 3003);                       // duplicate excluded
    CHECK(r.seq().duplicates() == 1);
    CHECK(r.negative_hw() == 1 && r.most_negative_hw_ns() == -25000);
    CHECK(r.hw().max() == 4900000 && r.hw().min() == -25000);
    CHECK(r.sw_missing() == 1);
    CHECK(r.div().count() == 3002);                   // all but the sample without a software stamp
    CHECK(r.div().min() == 800 && r.div().max() == 1000);  // base pattern is 800/900/1000
    CHECK(r.outlier_count() == LatencyRecorder::kTopK);
    CHECK(r.outlier(0).seq == 3000 && r.outlier(0).hw_lat_ns == 4900000);
    for (size_t i = 1; i < r.outlier_count(); ++i) CHECK(r.outlier(i - 1).hw_lat_ns >= r.outlier(i).hw_lat_ns);
    CHECK(r.active_window_s() > 3.0 && r.active_window_s() < 3.01);

    JsonLine j;
    r.append_summary(j);
    const std::string line = j.done();
    CHECK(std::count(line.begin(), line.end(), '{') == 1 && std::count(line.begin(), line.end(), '}') == 1);
    CHECK(line.find('[') == std::string::npos && line.find('\n') == std::string::npos);
    CHECK(line.find("\"negative_hw_samples\":1") != std::string::npos);
    CHECK(line.find("\"duplicate_count\":1") != std::string::npos);

    const std::string d = r.detail_json();
    CHECK(d.find("\"per_second\"") != std::string::npos && d.find("\"outliers\"") != std::string::npos);
    CHECK(std::count(d.begin(), d.end(), '{') == std::count(d.begin(), d.end(), '}'));
    CHECK(std::count(d.begin(), d.end(), '[') == std::count(d.begin(), d.end(), ']'));
}

static void test_warmup() {
    LatencyRecorder r;
    r.set_warmup_ns(2000000000LL);                 // 2 s
    const int64_t base = 1700000000000000000LL;
    // 5 s at 1 kpps; the first 1.9 s are slow, the rest are steady. Seq 10 is lost.
    for (uint32_t i = 0; i < 5000; ++i) {
        if (i == 10) continue;
        r.record(mk(i, base + i * 1000000LL, (i >= 1 && i < 1900) ? 900000 : 150000, 800));
    }
    CHECK(r.samples() == 3000);                    // 2000 warm-up packets left out of the latency stats
    CHECK(r.hw().max() == 150000);                 // none of the slow ones leaked in
    CHECK(r.seq().unique() == 4999);               // but every packet is still counted for loss
    JsonLine j;
    r.append_summary(j);
    const std::string line = j.done();
    CHECK(line.find("\"warmup_secs\":2.000") != std::string::npos);
    CHECK(line.find("\"warmup_skipped\":1999") != std::string::npos);   // 2000 minus the lost seq 10
    LatencyRecorder none;                          // disabled by default
    for (uint32_t i = 0; i < 100; ++i) none.record(mk(i, base + i * 1000000LL, 150000, 800));
    CHECK(none.samples() == 100);
}
static void test_json_escape() {
    JsonLine j;
    j.str("k", "a\"b\\c\n\x01");
    CHECK(j.done() == "{\"k\":\"a\\\"b\\\\c\\n\\u0001\"}");
}

int main() {
    test_bucket_geometry();
    test_quantiles_vs_exact();
    test_seq_tracker();
    test_recorder();
    test_warmup();
    test_json_escape();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
