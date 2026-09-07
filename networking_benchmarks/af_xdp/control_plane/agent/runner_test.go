package main

import (
	"encoding/json"
	"testing"

	"afxdp-cp/proto"
)

// The rtt tool writes this shape to /tmp/rtt_results.json; the parser must map
// it into proto.Metrics faithfully (this is the schema the whole matrix keys on).
func TestRttJSONParse(t *testing.T) {
	raw := `{
		"messages": 10000, "lost": 3, "loss_pct": 0.03, "clock_skew_samples": 0,
		"service_rtt_us": {"min": 18, "mean": 25, "p50": 24, "p90": 31, "p95": 34, "p99": 45, "p999": 88, "max": 210}
	}`
	var j rttJSON
	if err := json.Unmarshal([]byte(raw), &j); err != nil {
		t.Fatal(err)
	}
	m := toMetrics(j)
	if m.Messages != 10000 || m.Lost != 3 || m.LossPct != 0.03 {
		t.Fatalf("counters wrong: %+v", m)
	}
	if m.ServiceRTT.P50 != 24 || m.ServiceRTT.P99 != 45 || m.ServiceRTT.Max != 210 || m.ServiceRTT.Min != 18 {
		t.Fatalf("percentiles wrong: %+v", m.ServiceRTT)
	}
}

// mcastAbnormalTail must flag the SCHED_FIFO busy-poll stall shape
// (normal p50, tail latency orders of magnitude higher, often with 0% loss)
// and must NOT flag either a clean run or a genuinely saturated/queued run
// whose percentiles are all elevated proportionally - only the former should
// be retried, the latter is a real (if bad) measurement.
func TestMcastAbnormalTail(t *testing.T) {
	clean := rttJSON{Messages: 100000, Lost: 0, LossPct: 0}
	clean.Service.P50, clean.Service.Max = 34, 42
	if got := mcastAbnormalTail(clean); got != "" {
		t.Fatalf("clean run flagged as pathological: %q", got)
	}

	stall := rttJSON{Messages: 100000, Lost: 0, LossPct: 0}
	stall.Service.P50, stall.Service.Max = 34, 703618
	if got := mcastAbnormalTail(stall); got == "" {
		t.Fatal("stall shape (p50=34us max=703618us) not flagged")
	}

	// Saturated/queued: percentiles are all elevated together, not a
	// p50-vs-max outlier - must NOT be treated as the SCHED_FIFO stall.
	saturated := rttJSON{Messages: 100000, Lost: 2482, LossPct: 2.48}
	saturated.Service.P50, saturated.Service.Max = 5181, 5415
	if got := mcastAbnormalTail(saturated); got != "" {
		t.Fatalf("saturated run incorrectly flagged as stall: %q", got)
	}

	// No messages received at all: caller's TimeoutSec-expiry path already
	// handles this; must not also be flagged here.
	empty := rttJSON{Messages: 0}
	if got := mcastAbnormalTail(empty); got != "" {
		t.Fatalf("empty result incorrectly flagged: %q", got)
	}
}

// applyRateExpectation must compute the shortfall ratio correctly, and must
// leave RequestedPps/RateShortfall unset (proto zero values) when there is
// nothing to compare against - an unbounded-rate run (requestedPps<=0) or a
// receiver result with no achieved-rate data (older tool binary, or a run
// that received 0 packets).
func TestApplyRateExpectation(t *testing.T) {
	// Rate met: 95k achieved of 100k requested (>=90%) - not a shortfall.
	m := proto.Metrics{AchievedPps: 95000}
	applyRateExpectation(&m, 100000)
	if m.RequestedPps != 100000 {
		t.Fatalf("RequestedPps not set: %+v", m)
	}
	if m.RateShortfall {
		t.Fatalf("95%% of requested incorrectly flagged as shortfall: %+v", m)
	}

	// Rate NOT met: 50k achieved of 100k requested (<90%) - a shortfall,
	// matching dev/roadmap/fix.md's Error 3 (offered rate exceeding what mcast_send
	// could sustain).
	m2 := proto.Metrics{AchievedPps: 50000}
	applyRateExpectation(&m2, 100000)
	if !m2.RateShortfall {
		t.Fatalf("50%% of requested not flagged as shortfall: %+v", m2)
	}

	// No requested rate (unbounded run) - fields must stay unset, not a
	// nonsensical 0/0 ratio.
	m3 := proto.Metrics{AchievedPps: 50000}
	applyRateExpectation(&m3, 0)
	if m3.RequestedPps != 0 || m3.RateShortfall {
		t.Fatalf("unbounded-rate run incorrectly got rate fields set: %+v", m3)
	}

	// No achieved-rate data (e.g. older mcast_receive binary predating
	// elapsed_s/achieved_pps) - must not divide by/against zero.
	m4 := proto.Metrics{}
	applyRateExpectation(&m4, 100000)
	if m4.RequestedPps != 0 || m4.RateShortfall {
		t.Fatalf("missing achieved-rate data incorrectly got rate fields set: %+v", m4)
	}
}

// nicTuningAdaptive must correctly split ethtool -c's combined
// "Adaptive RX: off  TX: n/a" line into its two values - a naive line-prefix
// grep for "Adaptive RX:" pulls in the trailing "TX: n/a" text too, and one
// for "Adaptive TX:" never matches at all since that label never starts its
// own line.
func TestNicTuningAdaptive(t *testing.T) {
	text := "Coalesce parameters for enp39s0:\nAdaptive RX: off  TX: n/a\nstats-block-usecs: n/a\n"
	rx, tx := nicTuningAdaptive(text)
	if rx != "off" {
		t.Fatalf("rx: got %q, want %q", rx, "off")
	}
	if tx != "n/a" {
		t.Fatalf("tx: got %q, want %q", tx, "n/a")
	}

	// Missing line: must return ("", ""), not panic or return garbage.
	rx2, tx2 := nicTuningAdaptive("no adaptive line here\n")
	if rx2 != "" || tx2 != "" {
		t.Fatalf("expected empty strings for missing line, got (%q, %q)", rx2, tx2)
	}
}

func TestDerivePinsDefault(t *testing.T) {
	// Off-EC2 (no isolcpus in /proc/cmdline) -> lo defaults to 1 => send=4, recv=3
	// (IRQ=lo, poll=lo+1, recv=lo+2, send=lo+3).
	send, recv := derivePins()
	if recv >= send {
		t.Fatalf("recv (%d) should be below send (%d)", recv, send)
	}
	if send-recv != 1 {
		t.Fatalf("send/recv should be adjacent isolated cores, got send=%d recv=%d", send, recv)
	}
}

func TestParseCPUList(t *testing.T) {
	got := parseCPUList("1-4,6")
	want := []int{1, 2, 3, 4, 6}
	if len(got) != len(want) {
		t.Fatalf("want %v, got %v", want, got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("want %v, got %v", want, got)
		}
	}
	if len(parseCPUList("")) != 0 {
		t.Fatal("empty string should parse to empty list")
	}
	if x := parseCPUList("2"); len(x) != 1 || x[0] != 2 {
		t.Fatalf("single cpu parse wrong: %v", x)
	}
}
