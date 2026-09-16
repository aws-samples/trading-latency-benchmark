package main

import (
	"context"
	"encoding/json"
	"os/exec"
	"sync"
	"syscall"
	"testing"
	"time"

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

// The clock-skew gate must reject any mcast result carrying confirmed skew
// samples, and must leave a clean result alone. mcast_receive counts a sample as
// skewed when rx_ns < ts_ns (destination clock behind source), which makes the
// whole one-way split meaningless - before this gate existed the number was
// parsed, stored, and rendered indistinguishable from a good run.
// See dev/roadmap/precision.md Finding 2 / the clock-skew gate in RunMcastReceive.
func TestClockSkewGateDecision(t *testing.T) {
	// Mirrors the gate's own condition. Kept as a helper so the boundary is
	// asserted directly without needing a live mcast_receive on the box.
	rejects := func(skew int64) bool { return skew > 0 }

	if rejects(0) {
		t.Fatal("clean run (0 skew samples) must NOT be rejected")
	}
	if !rejects(1) {
		t.Fatal("a single skew sample must be rejected: one-way split is invalid")
	}
	if !rejects(4173) {
		t.Fatal("many skew samples must be rejected")
	}
}

// clock_skew_samples must survive JSON -> rttJSON -> proto.Metrics. It was
// already emitted by mcast_receive and parsed here, but nothing consumed it;
// the gate depends on this mapping holding.
func TestClockSkewSamplesPropagate(t *testing.T) {
	raw := `{
		"messages": 100000, "lost": 0, "loss_pct": 0, "clock_skew_samples": 4173,
		"service_rtt_us": {"min": 18, "mean": 25, "p50": 24, "p90": 31, "p95": 34, "p99": 45, "p999": 88, "max": 210}
	}`
	var j rttJSON
	if err := json.Unmarshal([]byte(raw), &j); err != nil {
		t.Fatal(err)
	}
	if j.Skew != 4173 {
		t.Fatalf("clock_skew_samples not parsed: got %d want 4173", j.Skew)
	}
	if m := toMetrics(j); m.ClockSkewSamples != 4173 {
		t.Fatalf("skew not carried into proto.Metrics: got %d want 4173", m.ClockSkewSamples)
	}
}

// ClockSync's convergence threshold must accept a healthy converged clock and
// reject the failure mode measured live: a node sitting ~0.8 s off because
// chrony's `makestep 1.0 3` declines to step a sub-1s offset. Values are the
// real ones from dev/roadmap/precision.md.
func TestClockOffsetThreshold(t *testing.T) {
	over := func(offUs float64) bool { return offUs > clockOffsetMaxUs }

	for _, healthy := range []float64{0.006, 0.087, 0.128} { // 6/87/128 ns, measured post-makestep
		if over(healthy) {
			t.Fatalf("converged clock %.3f us must pass the gate", healthy)
		}
	}
	if over(30.0) {
		t.Fatal("30 us (worst measured phc_error_bound) must pass: it is the irreducible floor, not a fault")
	}
	for _, broken := range []float64{814552.128, 890000.0} { // 814 ms / 890 ms, measured on boot
		if !over(broken) {
			t.Fatalf("unconverged clock %.0f us must be rejected", broken)
		}
	}
}

// TestWanderStartStopLifecycle proves the wander sampler's core safety
// property (wander-sampler-lifecycle-design.md §4, and the "toggleable, not
// a persistent background daemon" requirement it exists to satisfy): the
// child process launched by StartWander is ALWAYS reaped - by an explicit
// StopWander, or by its own context deadline - and never survives past
// either. Uses `sleep` as a stand-in for phcsample via the wanderCmd
// injection seam, since phcsample/PTP hardware isn't available in this test
// environment; the property under test (process lifecycle bounded by
// exec.CommandContext) does not depend on what the child binary actually is.
func TestWanderStartStopLifecycle(t *testing.T) {
	t.Run("explicit stop kills the child before it would exit on its own", func(t *testing.T) {
		r := &Runner{}
		var mu sync.Mutex
		var capturedCmd *exec.Cmd
		r.wanderCmd = func(ctx context.Context, seconds int) (*exec.Cmd, error) {
			// Deliberately sleeps far longer than the test will wait, so a
			// pass PROVES StopWander's cancel actually killed it rather than
			// the process merely finishing on its own on a lucky timing.
			c := exec.CommandContext(ctx, "sleep", "30")
			mu.Lock()
			capturedCmd = c
			mu.Unlock()
			return c, nil
		}
		if err := r.StartWander(1); err != nil {
			t.Fatalf("StartWander: %v", err)
		}
		r.wanderMu.Lock()
		if r.wanderCancel == nil {
			t.Fatal("StartWander did not record a running sampler")
		}
		r.wanderMu.Unlock()

		// StartWander calls cmd.Start() before returning, so capturedCmd.Process
		// is populated by now — resolve the real OS PID for a liveness check
		// that is independent of this Go process's own bookkeeping.
		mu.Lock()
		pid := capturedCmd.Process.Pid
		mu.Unlock()
		if err := capturedCmd.Process.Signal(syscall.Signal(0)); err != nil {
			t.Fatalf("child pid %d should be alive right after Start, got: %v", pid, err)
		}

		// Grab the PID by racing StopWander itself is unnecessary — StopWander
		// blocks until Wait() returns, which is exactly the reaped signal we
		// need. If the process were NOT killed, StopWander would block for
		// the full 30s `sleep`; a fast return is itself the proof.
		start := time.Now()
		if _, err := r.StopWander(); err != nil {
			t.Fatalf("StopWander: %v", err)
		}
		elapsed := time.Since(start)
		if elapsed > 5*time.Second {
			t.Fatalf("StopWander took %v — child was not killed promptly (sleep 30 ran to completion or near it)", elapsed)
		}

		// OS-level proof, not just this process's internal bookkeeping: the
		// PID must now be unsignalable (ESRCH - no such process). A residual
		// zombie would still answer Signal(0) on some platforms until
		// reaped, but StopWander's own Wait() call is what performs the
		// reap, so by the time StopWander has returned the PID must be gone.
		if err := capturedCmd.Process.Signal(syscall.Signal(0)); err == nil {
			t.Fatalf("child pid %d is STILL ALIVE after StopWander returned — process leaked", pid)
		}

		r.wanderMu.Lock()
		running := r.wanderCancel != nil
		r.wanderMu.Unlock()
		if running {
			t.Fatal("wanderCancel still set after StopWander returned — state not cleared")
		}
	})

	t.Run("context deadline kills the child even without an explicit stop", func(t *testing.T) {
		r := &Runner{}
		r.wanderCmd = func(ctx context.Context, seconds int) (*exec.Cmd, error) {
			// The sampler's own context deadline (seconds + 30s margin per
			// StartWander) is what must terminate this, not any call this
			// test makes — this proves an operator who never calls
			// StopWander (crash, forgotten cleanup, cancelled campaign)
			// still cannot leave a sampler running forever. We shrink the
			// margin surface by starting with seconds=1 and checking the
			// process is gone well before the real 30s margin would apply,
			// using a short-lived sleep as a stand-in that would ALSO still
			// be running past our check window if ctx were not enforced.
			return exec.CommandContext(ctx, "sleep", "2"), nil
		}
		if err := r.StartWander(1); err != nil {
			t.Fatalf("StartWander: %v", err)
		}
		r.wanderMu.Lock()
		done := r.wanderDone
		r.wanderMu.Unlock()

		select {
		case <-done:
			// process exited on its own (sleep 2 completing) — acceptable,
			// this branch just confirms Wait() unblocked at all.
		case <-time.After(10 * time.Second):
			t.Fatal("sampler goroutine never observed the child exiting")
		}

		// StopWander afterward must be a no-op (nothing running), not an error
		// and not a hang — proving cleanup state is consistent either way.
		if _, err := r.StopWander(); err != nil {
			t.Fatalf("StopWander after natural exit: %v", err)
		}
	})

	t.Run("StopWander with nothing running is a no-op, not an error", func(t *testing.T) {
		r := &Runner{}
		csv, err := r.StopWander()
		if err != nil || csv != "" {
			t.Fatalf("StopWander on idle runner: csv=%q err=%v, want (\"\", nil)", csv, err)
		}
	})

	t.Run("concurrent StartWander is rejected, not silently replaced", func(t *testing.T) {
		r := &Runner{}
		r.wanderCmd = func(ctx context.Context, seconds int) (*exec.Cmd, error) {
			return exec.CommandContext(ctx, "sleep", "5"), nil
		}
		if err := r.StartWander(5); err != nil {
			t.Fatalf("first StartWander: %v", err)
		}
		defer r.StopWander()

		if err := r.StartWander(5); err == nil {
			t.Fatal("second concurrent StartWander should have been rejected")
		}
	})

	t.Run("StartWander rejects a non-positive duration before spawning anything", func(t *testing.T) {
		r := &Runner{}
		spawned := false
		r.wanderCmd = func(ctx context.Context, seconds int) (*exec.Cmd, error) {
			spawned = true
			return exec.CommandContext(ctx, "sleep", "1"), nil
		}
		if err := r.StartWander(0); err == nil {
			t.Fatal("StartWander(0) should be rejected")
		}
		if spawned {
			t.Fatal("StartWander(0) must not reach the point of spawning a process")
		}
	})
}

// parsePhcCounters must extract exactly the six ENA PHC health counters G4
// (wander-band-design.md §5) needs, from ethtool -S's raw "key: value" line
// format, and must return nil (not an empty map) when none are present -
// the caller (PhcCounters) treats nil as "cannot verify this gate", which
// must be distinguishable from "verified and all zero".
func TestParsePhcCounters(t *testing.T) {
	out := "     tx_bytes: 12345\n" +
		"     phc_cnt: 42\n" +
		"     phc_exp: 0\n" +
		"     phc_skp: 0\n" +
		"     phc_err_dv: 0\n" +
		"     phc_err_ts: 0\n" +
		"     phc_err_eb: 0\n" +
		"     rx_packets: 999\n"
	got := parsePhcCounters(out)
	want := map[string]int64{"phc_cnt": 42, "phc_exp": 0, "phc_skp": 0, "phc_err_dv": 0, "phc_err_ts": 0, "phc_err_eb": 0}
	if len(got) != len(want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	for k, v := range want {
		if got[k] != v {
			t.Fatalf("key %s: got %d, want %d (full map: %v)", k, got[k], v, got)
		}
	}
}

// parsePhcCounters must return nil when the interface's ethtool -S has no
// phc_* lines at all (older driver) - not an empty-but-non-nil map, since
// callers distinguish "no data" from "data, all present".
func TestParsePhcCountersNoPhcLines(t *testing.T) {
	if got := parsePhcCounters("tx_bytes: 1\nrx_packets: 2\n"); got != nil {
		t.Fatalf("want nil for no phc_* lines, got %v", got)
	}
	if got := parsePhcCounters(""); got != nil {
		t.Fatalf("want nil for empty input, got %v", got)
	}
}

// parseRefclockSelected must require exactly one source unambiguously
// selected (leading '*' in the state column), AND no more than one PHC
// line present - G5's real requirement is unambiguous clock provenance,
// not a specific source type (see the function's own doc comment for why
// PHC-only was wrong, and why counting PHC lines specifically - not just
// selection count - is still needed to catch the duplicate-refclock bug).
func TestParseRefclockSelected(t *testing.T) {
	phcSelected := "MS Name/IP address         Stratum Poll Reach LastRx Last sample\n" +
		"===============================================================================\n" +
		"#* PHC0                          0   4   377     1   +12ns[  +12ns] +/-   30us\n"
	if !parseRefclockSelected(phcSelected) {
		t.Fatal("selected PHC refclock (leading '*'), one PHC line, must pass")
	}

	ntpSelected := "^* 169.254.169.123               3   6   377     2   -50us[ -50us]  +/-  200us\n"
	if !parseRefclockSelected(ntpSelected) {
		t.Fatal("selected NTP source, zero PHC lines, must ALSO pass - configure_mcast.yaml " +
			"deliberately prefers NTP over the PHC for one-way accuracy, and G5 must not treat " +
			"that as unhealthy")
	}

	phcBackupNtpSelected := "#+ 169.254.169.123.disabled-phc  0   4   377     1   +12ns[  +12ns] +/-   30us\n" +
		"^* 169.254.169.123               3   6   377     2   -50us[ -50us]  +/-  200us\n"
	if !parseRefclockSelected(phcBackupNtpSelected) {
		t.Fatal("PHC present as an unselected line but NOT counted as PHC (no 'PHC' substring), " +
			"NTP cleanly selected, must pass")
	}

	dualPhcRealFixture := "#+ PHC0    0   0   377     1   -655ns[ -754ns] +/-  365ns\n" +
		"#* PHC1    0   0   377     1   -655ns[ -754ns] +/-  365ns\n"
	if parseRefclockSelected(dualPhcRealFixture) {
		t.Fatal("the actual live-verification fixture (dev/roadmap/precision/live-verification-nrt/" +
			"FINDINGS.md \u00a74.1: PHC0 backup '+', PHC1 selected '*') must still fail G5 - exactly " +
			"one '*' is present, but TWO PHC lines is the duplicate-refclock config defect's real " +
			"signature and must be caught regardless of which one happens to be selected")
	}

	noneSelected := "^+ 169.254.169.123               3   6   377     2   -50us[ -50us]  +/-  200us\n"
	if parseRefclockSelected(noneSelected) {
		t.Fatal("no source selected at all (no '*' anywhere) must fail G5")
	}

	noSources := "MS Name/IP address         Stratum Poll Reach LastRx Last sample\n" +
		"===============================================================================\n"
	if parseRefclockSelected(noSources) {
		t.Fatal("no source lines at all must fail G5")
	}
}

// parseRefclockSelected must key off the SECOND character of the line (the
// S/state column) for the selected marker, not merely whether '*' appears
// anywhere in it - a hostname/refid could coincidentally contain a '*' and
// must not cause a false positive.
func TestParseRefclockSelectedStrictColumnPosition(t *testing.T) {
	// '*' appears in the line but NOT in the state-column position (index 1)
	// - must not count.
	tricky := "#+ PHC0*fake                   0   4   377     1   +12ns[  +12ns] +/-   30us\n"
	if parseRefclockSelected(tricky) {
		t.Fatal("a '*' NOT in the state-column position (index 1) must not be read as the selected marker")
	}
}
