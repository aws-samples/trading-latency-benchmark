// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: MIT-0
//
// lat_stats.hpp - bounded-memory latency statistics for mcast_recv.
//
// Header-only and portable (no Linux APIs) so the logic can be unit-tested
// anywhere. Contents:
//   LogHist / SignedHist - log-linear histograms (1/1024 relative bucket width)
//   SeqTracker           - duplicate / out-of-order accounting per sequence number
//   LatencyRecorder      - per-packet recording: hardware stamp is primary, the
//                          kernel software stamp feeds the divergence statistics
//   JsonLine             - flat single-line JSON writer (see compatibility note)
//
// Compatibility note: save_report.py extracts a result with the regex
// `(i-\S+) multicast loss: (\{.*?\})`, which stops at the first '}'. The summary
// line therefore must be one flat object. Nested data goes in the detail file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace latstats {

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Flat single-line JSON object. Values are scalars only.
class JsonLine {
public:
    JsonLine() : s_("{") {}
    JsonLine& str(const char* k, const std::string& v) {
        key(k);
        s_ += '"' + json_escape(v) + '"';
        return *this;
    }
    JsonLine& i64(const char* k, int64_t v) {
        key(k);
        s_ += std::to_string(v);
        return *this;
    }
    JsonLine& u64(const char* k, uint64_t v) {
        key(k);
        s_ += std::to_string(v);
        return *this;
    }
    JsonLine& boolean(const char* k, bool v) {
        key(k);
        s_ += v ? "true" : "false";
        return *this;
    }
    // Nanoseconds rendered as microseconds with 3 decimals.
    JsonLine& us(const char* k, int64_t ns) {
        key(k);
        char b[48];
        snprintf(b, sizeof b, "%.3f", static_cast<double>(ns) / 1000.0);
        s_ += b;
        return *this;
    }
    JsonLine& f3(const char* k, double v) {
        key(k);
        char b[48];
        snprintf(b, sizeof b, "%.3f", v);
        s_ += b;
        return *this;
    }
    std::string done() const { return s_ + "}"; }

private:
    void key(const char* k) {
        if (!first_) s_ += ',';
        first_ = false;
        s_ += '"';
        s_ += k;
        s_ += "\":";
    }
    std::string s_;
    bool first_ = true;
};

// ---------------------------------------------------------------------------
// Log-linear histogram over magnitudes (nanoseconds)
// ---------------------------------------------------------------------------
//
// Values below 2^SUB_BITS are exact. Above that each power-of-two octave is cut
// into 2^SUB_BITS equal buckets, so the bucket width is at most 1/1024 of the
// value (0.098%) and a bucket-midpoint estimate is within 0.049%. Values at or
// above 2^MAX_BITS (~4.9 h) saturate into the last bucket.

class LogHist {
public:
    static constexpr int SUB_BITS = 10;
    static constexpr uint64_t SUB = 1ULL << SUB_BITS;
    static constexpr int MAX_BITS = 44;
    static constexpr size_t NBUCKETS = static_cast<size_t>(MAX_BITS - SUB_BITS + 1) * SUB;

    LogHist() : c_(NBUCKETS, 0), total_(0) {}

    static size_t index(uint64_t v) {
        if (v < SUB) return static_cast<size_t>(v);
        if (v >= (1ULL << MAX_BITS)) return NBUCKETS - 1;
        const int msb = 63 - __builtin_clzll(v);
        const int shift = msb - SUB_BITS;
        const uint64_t top = v >> shift;
        return static_cast<size_t>((static_cast<uint64_t>(shift) + 1) * SUB + (top - SUB));
    }
    static uint64_t lower(size_t idx) {
        if (idx < SUB) return idx;
        const unsigned shift = static_cast<unsigned>(idx / SUB) - 1;
        const uint64_t top = SUB + (idx % SUB);
        return top << shift;
    }
    static uint64_t width(size_t idx) {
        return idx < SUB ? 1 : (1ULL << (static_cast<unsigned>(idx / SUB) - 1));
    }
    static uint64_t representative(size_t idx) {
        const uint64_t w = width(idx);
        return lower(idx) + (w > 1 ? w / 2 : 0);
    }

    void add(uint64_t v) {
        ++c_[index(v)];
        ++total_;
    }
    uint64_t total() const { return total_; }
    const std::vector<uint64_t>& counts() const { return c_; }

    // Bucket holding the r-th smallest magnitude (0-based, r < total()).
    size_t bucket_of_rank(uint64_t r) const {
        uint64_t acc = 0;
        for (size_t i = 0; i < c_.size(); ++i) {
            acc += c_[i];
            if (acc > r) return i;
        }
        return c_.size() - 1;
    }

private:
    std::vector<uint64_t> c_;
    uint64_t total_;
};

// Signed values: negatives are kept as magnitudes in a second histogram, so a
// skewed clock never gets clamped or hidden.
class SignedHist {
public:
    void add(int64_t v) {
        ++n_;
        sum_ += static_cast<long double>(v);
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
        if (v >= 0) pos_.add(static_cast<uint64_t>(v));
        else neg_.add(static_cast<uint64_t>(0) - static_cast<uint64_t>(v));
    }
    uint64_t count() const { return n_; }
    int64_t min() const { return n_ ? min_ : 0; }
    int64_t max() const { return n_ ? max_ : 0; }
    double mean() const { return n_ ? static_cast<double>(sum_ / static_cast<long double>(n_)) : 0.0; }

    // k-th smallest value, 0-based.
    int64_t value_at_rank(uint64_t k) const {
        if (!n_) return 0;
        const uint64_t nneg = neg_.total();
        int64_t v;
        if (k < nneg) {
            const uint64_t m = nneg - 1 - k;  // ascending value == descending magnitude
            v = -static_cast<int64_t>(LogHist::representative(neg_.bucket_of_rank(m)));
        } else {
            v = static_cast<int64_t>(LogHist::representative(pos_.bucket_of_rank(k - nneg)));
        }
        return std::min(std::max(v, min_), max_);
    }
    // Rank rule: sorted[min(int(n*q), n-1)].
    int64_t quantile(double q) const {
        if (!n_) return 0;
        uint64_t k = static_cast<uint64_t>(static_cast<double>(n_) * q);
        if (k >= n_) k = n_ - 1;
        return value_at_rank(k);
    }

    // Sparse dump that a consumer can merge across subscribers and re-quantile.
    std::string to_json() const {
        std::string s = "{\"sub_bits\":" + std::to_string(LogHist::SUB_BITS) +
                        ",\"max_bits\":" + std::to_string(LogHist::MAX_BITS) +
                        ",\"unit\":\"ns\",\"count\":" + std::to_string(n_);
        char b[64];
        snprintf(b, sizeof b, ",\"sum\":%.0Lf", sum_);
        s += b;
        s += ",\"min\":" + std::to_string(min()) + ",\"max\":" + std::to_string(max());
        s += ",\"pos\":" + sparse(pos_) + ",\"neg\":" + sparse(neg_) + "}";
        return s;
    }

private:
    static std::string sparse(const LogHist& h) {
        std::string s = "[";
        bool first = true;
        const auto& c = h.counts();
        for (size_t i = 0; i < c.size(); ++i) {
            if (!c[i]) continue;
            if (!first) s += ',';
            first = false;
            s += "[" + std::to_string(i) + "," + std::to_string(c[i]) + "]";
        }
        return s + "]";
    }
    LogHist pos_, neg_;
    uint64_t n_ = 0;
    int64_t min_ = INT64_MAX, max_ = INT64_MIN;
    long double sum_ = 0;
};

// ---------------------------------------------------------------------------
// Sequence accounting
// ---------------------------------------------------------------------------

class SeqTracker {
public:
    struct Result {
        bool duplicate;
        bool out_of_order;
        bool tracked;
    };
    static constexpr uint64_t kCapBits = 1ULL << 30;   // 128 MiB bitmap ceiling
    static constexpr int64_t kMaxJump = 1LL << 24;      // implausible forward jump guard

    Result observe(uint32_t seq) {
        if (seq >= kCapBits) {
            ++untracked_;
            return {false, false, false};
        }
        const size_t w = static_cast<size_t>(seq >> 6);
        if (w >= bits_.size()) {
            // Growth is bounded relative to the highest sequence seen (or zero for
            // the first packet), so one corrupt packet cannot force a huge bitmap.
            const int64_t base = max_seq_ > 0 ? max_seq_ : 0;
            if (static_cast<int64_t>(seq) > base + kMaxJump) {
                ++untracked_;
                return {false, false, false};
            }
            bits_.resize(std::max(bits_.size() * 2, w + 1024), 0);
        }
        const uint64_t m = 1ULL << (seq & 63);
        if (bits_[w] & m) {
            ++dup_;
            return {true, false, true};
        }
        bits_[w] |= m;
        ++unique_;
        const bool ooo = max_seq_ >= 0 && static_cast<int64_t>(seq) < max_seq_;
        if (ooo) ++ooo_;
        if (static_cast<int64_t>(seq) > max_seq_) max_seq_ = static_cast<int64_t>(seq);
        return {false, ooo, true};
    }
    uint64_t unique() const { return unique_; }
    uint64_t duplicates() const { return dup_; }
    uint64_t out_of_order() const { return ooo_; }
    uint64_t untracked() const { return untracked_; }
    int64_t max_seq() const { return max_seq_; }

private:
    std::vector<uint64_t> bits_;
    uint64_t unique_ = 0, dup_ = 0, ooo_ = 0, untracked_ = 0;
    int64_t max_seq_ = -1;
};

// ---------------------------------------------------------------------------
// Per-packet recorder
// ---------------------------------------------------------------------------

struct Sample {
    uint32_t seq;
    int64_t tx_ns;   // sender CLOCK_REALTIME, from the payload
    int64_t hw_ns;   // NIC hardware receive stamp (PHC domain) - primary
    int64_t sw_ns;   // kernel software receive stamp (CLOCK_REALTIME) - divergence
    bool has_sw;
};

class LatencyRecorder {
public:
    static constexpr size_t kTopK = 16;
    static constexpr size_t kMaxSeconds = 7200;
    static constexpr double kQuantileErrorPct = 0.05;

    struct Outlier {
        uint32_t seq;
        int64_t hw_lat_ns;
        int64_t sw_lat_ns;
        bool has_sw;
        int64_t rel_ns;  // since the first recorded packet
    };

    // Packets whose hardware stamp falls within `ns` of the first one are still counted for
    // loss, duplicates and ordering, but their latency is left out of every statistic.
    void set_warmup_ns(int64_t ns) { warmup_ns_ = ns; }
    void record(const Sample& s) {
        const SeqTracker::Result r = seq_.observe(s.seq);
        if (r.duplicate) return;  // a second copy is not a delivery; counted only
        if (warmup_ns_ > 0) {
            if (!warm_started_) {
                warm_started_ = true;
                warm_first_hw_ns_ = s.hw_ns;
            }
            if (s.hw_ns - warm_first_hw_ns_ < warmup_ns_) {
                ++warmup_skipped_;
                return;
            }
        }

        const int64_t lat_hw = s.hw_ns - s.tx_ns;
        if (samples_ == 0) first_hw_ns_ = s.hw_ns;
        if (s.hw_ns > last_hw_ns_) last_hw_ns_ = s.hw_ns;
        ++samples_;

        hw_.add(lat_hw);
        if (lat_hw < 0) {
            ++neg_hw_;
            if (lat_hw < most_neg_hw_) most_neg_hw_ = lat_hw;
        }
        if (samples_ <= 100 && lat_hw > first100_max_) first100_max_ = lat_hw;

        int64_t lat_sw = 0, div = 0;
        if (s.has_sw) {
            lat_sw = s.sw_ns - s.tx_ns;
            div = s.sw_ns - s.hw_ns;
            sw_.add(lat_sw);
            div_.add(div);
            if (lat_sw < 0) ++neg_sw_;
        } else {
            ++sw_missing_;
        }

        const int64_t rel = s.hw_ns - first_hw_ns_;
        size_t sec = rel > 0 ? static_cast<size_t>(rel / 1000000000LL) : 0;
        if (sec >= kMaxSeconds) sec = kMaxSeconds - 1;
        if (sec >= sec_.size()) sec_.resize(sec + 1);
        SecBucket& b = sec_[sec];
        ++b.n;
        if (lat_hw > b.max_hw) b.max_hw = lat_hw;
        if (s.has_sw) {
            if (lat_sw > b.max_sw) b.max_sw = lat_sw;
            if (div > b.max_div) b.max_div = div;
        }

        if (topk_n_ < kTopK || lat_hw > topk_[kTopK - 1].hw_lat_ns) {
            Outlier o{s.seq, lat_hw, lat_sw, s.has_sw, rel};
            if (topk_n_ < kTopK) topk_[topk_n_++] = o;
            else topk_[kTopK - 1] = o;
            for (size_t i = topk_n_ - 1; i > 0 && topk_[i].hw_lat_ns > topk_[i - 1].hw_lat_ns; --i)
                std::swap(topk_[i], topk_[i - 1]);
        }
    }

    const SeqTracker& seq() const { return seq_; }
    const SignedHist& hw() const { return hw_; }
    const SignedHist& sw() const { return sw_; }
    const SignedHist& div() const { return div_; }
    uint64_t samples() const { return samples_; }
    uint64_t negative_hw() const { return neg_hw_; }
    uint64_t negative_sw() const { return neg_sw_; }
    uint64_t sw_missing() const { return sw_missing_; }
    int64_t most_negative_hw_ns() const { return neg_hw_ ? most_neg_hw_ : 0; }
    double active_window_s() const {
        return samples_ > 1 ? static_cast<double>(last_hw_ns_ - first_hw_ns_) / 1e9 : 0.0;
    }
    size_t outlier_count() const { return topk_n_; }
    const Outlier& outlier(size_t i) const { return topk_[i]; }

    // Latency, divergence and accounting fields of the flat summary line.
    void append_summary(JsonLine& j) const {
        j.u64("latency_samples", samples_);
        j.f3("warmup_secs", static_cast<double>(warmup_ns_) / 1e9);
        j.u64("warmup_skipped", warmup_skipped_);
        if (samples_) {
            j.us("min_latency_us", hw_.min()).us("max_latency_us", hw_.max());
            j.f3("mean_latency_us", hw_.mean() / 1000.0);
            j.us("median_latency_us", hw_.quantile(0.50));
            j.us("p95_latency_us", hw_.quantile(0.95));
            j.us("p99_latency_us", hw_.quantile(0.99));
            j.us("p999_latency_us", hw_.quantile(0.999));
            j.us("first100_max_latency_us", first100_max_);
        }
        if (sw_.count()) {
            j.us("sw_min_latency_us", sw_.min()).us("sw_max_latency_us", sw_.max());
            j.f3("sw_mean_latency_us", sw_.mean() / 1000.0);
            j.us("sw_median_latency_us", sw_.quantile(0.50));
            j.us("sw_p95_latency_us", sw_.quantile(0.95));
            j.us("sw_p99_latency_us", sw_.quantile(0.99));
            j.us("sw_p999_latency_us", sw_.quantile(0.999));
            // div = software stamp minus hardware stamp, per packet: receive-path
            // delay plus any clock-domain offset between CLOCK_REALTIME and the PHC.
            j.us("div_min_us", div_.min()).us("div_p1_us", div_.quantile(0.01));
            j.us("div_median_us", div_.quantile(0.50));
            j.f3("div_mean_us", div_.mean() / 1000.0);
            j.us("div_p99_us", div_.quantile(0.99)).us("div_max_us", div_.max());
        }
        j.u64("sw_timestamp_missing", sw_missing_);
        j.u64("negative_hw_samples", neg_hw_).us("most_negative_hw_us", most_negative_hw_ns());
        j.u64("negative_sw_samples", neg_sw_);
        j.u64("duplicate_count", seq_.duplicates());
        j.u64("out_of_order_count", seq_.out_of_order());
        j.u64("untracked_seq_count", seq_.untracked());
        j.f3("quantile_error_bound_pct", kQuantileErrorPct);
        j.f3("active_window_s", active_window_s());
        const double w = active_window_s();
        j.f3("achieved_rx_pps", w > 0 ? static_cast<double>(samples_ - 1) / w : 0.0);
    }

    // Nested detail document (histograms, outliers, per-second maxima).
    std::string detail_json() const {
        std::string s = "{\"schema_version\":1";
        s += ",\"hist_hw_latency\":" + hw_.to_json();
        s += ",\"hist_sw_latency\":" + sw_.to_json();
        s += ",\"hist_div_sw_minus_hw\":" + div_.to_json();
        s += ",\"outliers\":[";
        for (size_t i = 0; i < topk_n_; ++i) {
            const Outlier& o = topk_[i];
            char b[256];
            snprintf(b, sizeof b,
                     "%s{\"seq\":%u,\"hw_latency_us\":%.3f,\"sw_latency_us\":%s,\"since_first_ms\":%.3f}",
                     i ? "," : "", o.seq, o.hw_lat_ns / 1000.0,
                     o.has_sw ? (std::to_string(o.sw_lat_ns / 1000.0)).c_str() : "null",
                     o.rel_ns / 1e6);
            s += b;
        }
        s += "],\"per_second\":{";
        s += "\"n\":" + series([](const SecBucket& b) { return std::to_string(b.n); });
        s += ",\"max_hw_us\":" + series([](const SecBucket& b) { return us_or_null(b.n, b.max_hw); });
        s += ",\"max_sw_us\":" + series([](const SecBucket& b) { return us_or_null(b.n, b.max_sw); });
        s += ",\"max_div_us\":" + series([](const SecBucket& b) { return us_or_null(b.n, b.max_div); });
        s += "}}";
        return s;
    }

private:
    struct SecBucket {
        uint32_t n = 0;
        int64_t max_hw = INT64_MIN, max_sw = INT64_MIN, max_div = INT64_MIN;
    };
    static std::string us_or_null(uint32_t n, int64_t ns) {
        if (!n || ns == INT64_MIN) return "null";
        char b[48];
        snprintf(b, sizeof b, "%.3f", static_cast<double>(ns) / 1000.0);
        return b;
    }
    template <class F>
    std::string series(F f) const {
        std::string s = "[";
        for (size_t i = 0; i < sec_.size(); ++i) {
            if (i) s += ',';
            s += f(sec_[i]);
        }
        return s + "]";
    }

    SeqTracker seq_;
    SignedHist hw_, sw_, div_;
    uint64_t samples_ = 0, neg_hw_ = 0, neg_sw_ = 0, sw_missing_ = 0;
    int64_t warmup_ns_ = 0, warm_first_hw_ns_ = 0;
    bool warm_started_ = false;
    uint64_t warmup_skipped_ = 0;
    int64_t most_neg_hw_ = 0;
    int64_t first_hw_ns_ = 0, last_hw_ns_ = 0;
    int64_t first100_max_ = INT64_MIN;
    Outlier topk_[kTopK];
    size_t topk_n_ = 0;
    std::vector<SecBucket> sec_;
};

}  // namespace latstats
