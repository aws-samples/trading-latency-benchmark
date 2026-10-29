// Package wander reduces phcsample's raw CSV output into the per-campaign
// Wander analysis for PHC-disciplined clock offset samples. Pure computation
// only - no NATS/agent/store dependency, no process lifecycle. The
// orchestrator collects the raw CSV (Stage 2, CmdWanderStart/CmdWanderStop)
// and calls Reduce on it; a later stage attaches Fields to the run's stored
// params and evaluates the gates.
package wander

import (
	"encoding/csv"
	"fmt"
	"io"
	"math"
	"sort"
	"strconv"
	"strings"
)

// Sample is one parsed phcsample CSV row (seq,wall_ns,phc_ns,sysmid_ns,
// offset_ns,bracket_ns,eb_ns). A sentinel row (ioctl failure) has
// OffsetNs/BracketNs/EbNs all -1 and Sentinel=true - phcsample's own
// convention, preserved here rather than re-derived from the -1 values so a
// genuine (if implausible) -1 offset can never be silently mistaken for a
// sentinel.
type Sample struct {
	Seq       int64
	WallNs    int64
	PhcNs     int64
	SysmidNs  int64
	OffsetNs  int64
	BracketNs int64
	EbNs      int64
	Sentinel  bool
}

// Fields is the per-node reduction defined in wander-band-design.md §4.
// Both MAD forms are stored explicitly and separately - the doc's own
// documented trap (§4: analyze.py's "mad" column is SCALED, and every
// published wander figure in FINDINGS.md/delay-uncertainty-ledger.md is a
// scaled value; comparing a raw MAD against those under-reports the band by
// 1.4826x) is guarded by golden-file tests below, not just by convention.
type Fields struct {
	WanderMedNs           float64 `json:"wander_med_ns"`
	WanderMadRawNs        float64 `json:"wander_mad_raw_ns"`
	WanderMadScaledNs     float64 `json:"wander_mad_scaled_ns"`
	WanderMadPerSqrtSecNs float64 `json:"wander_mad_per_sqrt_sec_ns"`
	WanderDurationSec     float64 `json:"wander_duration_sec"`
	WanderP1Ns            float64 `json:"wander_p1_ns"`
	WanderP99Ns           float64 `json:"wander_p99_ns"`
	WanderExcursionPct    float64 `json:"wander_excursion_pct"`
	WanderStepCount       int     `json:"wander_step_count"`
	BracketMedNs          float64 `json:"bracket_med_ns"`
	EbMedNs               float64 `json:"eb_med_ns"`
	SampleCount           int     `json:"sample_count"`
	SentinelCount         int     `json:"sentinel_count"`
}

// madScale is the normal-consistency scaling factor for MAD-as-sigma-
// estimator (the wander_mad_scaled_ns definition). This exact constant
// matches the reference analysis implementation, keeping this
// implementation's numbers comparable to figures already published
// elsewhere for this fleet.
const madScale = 1.4826

// WanderMadPerSqrtSecNs (wander_mad_per_sqrt_sec_ns) exists because raw
// WanderMadScaledNs is not comparable across runs of different length. A
// short window under-samples chronyd's discipline-loop period and produces
// an artificially small MAD - not because the clock is quieter, but because
// the window is too short to see a full correction cycle. Dividing by
// sqrt(actual sampled duration) - the scaling a diffusive/random-walk
// process's dispersion follows over an observation window - substantially
// narrows the run-length-driven spread, though it is a partial correction:
// duration explains some of the spread, not most of it. The residual is
// genuine node/run-specific instability (chronyd correction events - see
// GateExcursionNorm/G6) that normalization must not be expected to remove.
// Both fields are kept: WanderMadScaledNs remains the doc's own defined
// quantity (wander-band-design.md §4) for FINDINGS.md/ledger comparability;
// WanderMadPerSqrtSecNs is a secondary diagnostic for comparing runs of
// different duration against each other, never a replacement for the gates.

// excursionThresholdNs is wander_excursion_pct's cutoff: the design doc (§1,
// §4) reports "~21% of samples beyond 3us" as the fleet baseline this field
// is meant to reproduce, so 3us is the threshold that figure was computed
// against, not an arbitrary choice.
const excursionThresholdNs = 3_000

// stepThresholdNs is wander_step_count's per-DELTA cutoff (not an absolute
// offset threshold - a step is a DISCONTINUITY between consecutive samples,
// distinct from excursionThresholdNs which flags a single large absolute
// sample). A step must invalidate the whole campaign; the design does not
// specify an exact number. 10us is chosen as a materially larger jump than
// ordinary wander on this fleet - ordinary wander stays within +/-4.7us at
// p99, so a 10us consecutive-sample jump is well outside anything wander
// alone produces and is a reasonable, documented judgment call rather than
// an arbitrary one.
const stepThresholdNs = 10_000

// ParseCSV parses phcsample's stdout (the header line plus one row per
// sample) into Samples. Malformed rows are skipped rather than failing the
// whole parse - a partially-corrupted CSV (e.g. truncated by a killed
// sampler) should still yield whatever valid rows it has, since Reduce's own
// coverage gate (sample_count) is what should reject an under-sampled run,
// not a parse error on one bad line.
func ParseCSV(r io.Reader) ([]Sample, error) {
	cr := csv.NewReader(r)
	cr.FieldsPerRecord = -1 // rows are fixed-width by convention, but don't hard-fail on ragged input
	rows, err := cr.ReadAll()
	if err != nil {
		return nil, fmt.Errorf("wander: csv read: %w", err)
	}
	var out []Sample
	for _, row := range rows {
		if len(row) == 0 {
			continue
		}
		// Header line: "seq,wall_ns,phc_ns,sysmid_ns,offset_ns,bracket_ns,eb_ns".
		// Skip by checking the first field isn't numeric, rather than
		// matching the literal header text - robust to a header phcsample
		// might reformat later.
		if _, err := strconv.ParseInt(strings.TrimSpace(row[0]), 10, 64); err != nil {
			continue
		}
		if len(row) < 7 {
			continue
		}
		s, ok := parseRow(row)
		if !ok {
			continue
		}
		out = append(out, s)
	}
	return out, nil
}

func parseRow(row []string) (Sample, bool) {
	var vals [7]int64
	for i := 0; i < 7; i++ {
		v, err := strconv.ParseInt(strings.TrimSpace(row[i]), 10, 64)
		if err != nil {
			return Sample{}, false
		}
		vals[i] = v
	}
	s := Sample{Seq: vals[0], WallNs: vals[1], PhcNs: vals[2], SysmidNs: vals[3],
		OffsetNs: vals[4], BracketNs: vals[5], EbNs: vals[6]}
	// phcsample's sentinel row is exactly phc_ns=-1 (see phcsample.c's ioctl
	// failure path: "%lld,%lld,-1,-1,-1,-1,-1\n"). Checking PhcNs specifically
	// (rather than OffsetNs) means a hypothetical future row shape that keeps
	// some fields valid on partial failure still gets classified correctly
	// against THIS field, which phcsample never sets to a real -1 in a
	// successful row (a PHC time in 1970 is not a value the driver returns).
	if s.PhcNs == -1 {
		s.Sentinel = true
	}
	return s, true
}

// Reduce computes wander-band-design.md §4's Fields from a node's parsed
// samples. Sentinel rows are excluded from every statistic except
// SampleCount/SentinelCount themselves, per §4's "excluding sentinel rows"
// instruction. An all-sentinel or empty input returns a zero Fields with
// SampleCount=0 - callers (the §5 gates, not yet implemented) are
// responsible for rejecting on that, Reduce itself never errors.
func Reduce(samples []Sample) Fields {
	f := Fields{SampleCount: len(samples)}
	var offsets, brackets, ebs []float64
	for _, s := range samples {
		if s.Sentinel {
			f.SentinelCount++
			continue
		}
		offsets = append(offsets, float64(s.OffsetNs))
		brackets = append(brackets, float64(s.BracketNs))
		ebs = append(ebs, float64(s.EbNs))
	}
	if len(offsets) == 0 {
		return f
	}

	med := nearestRankPercentile(offsets, 50)
	f.WanderMedNs = med
	absDevs := make([]float64, len(offsets))
	for i, x := range offsets {
		absDevs[i] = math.Abs(x - med)
	}
	f.WanderMadRawNs = nearestRankPercentile(absDevs, 50)
	f.WanderMadScaledNs = f.WanderMadRawNs * madScale

	// Duration normalization (see WanderMadPerSqrtSecNs's doc comment).
	// ActualDurationSec is computed over the full samples slice (including
	// sentinels), matching G1's own basis - the window a reader cares about
	// is what the sampler actually spanned in wall-clock time, not how many
	// of those rows happened to parse as real offsets.
	f.WanderDurationSec = ActualDurationSec(samples)
	if f.WanderDurationSec > 0 {
		f.WanderMadPerSqrtSecNs = f.WanderMadScaledNs / math.Sqrt(f.WanderDurationSec)
	}

	f.WanderP1Ns = nearestRankPercentile(offsets, 1)
	f.WanderP99Ns = nearestRankPercentile(offsets, 99)

	exCount := 0
	for _, x := range offsets {
		if math.Abs(x) > excursionThresholdNs {
			exCount++
		}
	}
	f.WanderExcursionPct = 100.0 * float64(exCount) / float64(len(offsets))

	// Step count: consecutive-sample DELTA beyond stepThresholdNs. Computed
	// over the ORIGINAL sample order (including sentinel gaps as
	// non-adjacent, since a sentinel row carries no real offset to diff
	// against) - not over the sentinel-filtered `offsets` slice, so a step
	// straddling a sentinel gap is still detected using the two real
	// samples that bracket it, matching how such a gap would actually look
	// to a reader of the raw CSV.
	f.WanderStepCount = countSteps(samples)

	f.BracketMedNs = nearestRankPercentile(brackets, 50)
	f.EbMedNs = nearestRankPercentile(ebs, 50)
	return f
}

// countSteps counts consecutive PAIRS of non-sentinel samples whose offset
// delta exceeds stepThresholdNs. Sentinel rows are skipped when forming
// pairs (not treated as a step themselves) - a gap in sampling is not the
// same event as a clock step, and conflating them would flag every EBUSY
// collision as if chronyd had stepped the clock.
func countSteps(samples []Sample) int {
	count := 0
	havePrev := false
	var prev float64
	for _, s := range samples {
		if s.Sentinel {
			continue
		}
		cur := float64(s.OffsetNs)
		if havePrev && math.Abs(cur-prev) > stepThresholdNs {
			count++
		}
		prev = cur
		havePrev = true
	}
	return count
}

// nearestRankPercentile matches the reference analysis implementation's
// pctl(): never interpolates, so every reported figure is a value that was
// actually measured. q is 0-100.
func nearestRankPercentile(xs []float64, q float64) float64 {
	if len(xs) == 0 {
		return 0
	}
	s := make([]float64, len(xs))
	copy(s, xs)
	sort.Float64s(s)
	idx := int(math.Round(q / 100.0 * float64(len(s)-1)))
	if idx < 0 {
		idx = 0
	}
	if idx > len(s)-1 {
		idx = len(s) - 1
	}
	return s[idx]
}

// PairwiseBand computes the quadrature upper bound for a measurement between
// two nodes, per wander-band-design.md §4's "The pairwise band":
// sqrt(madA^2 + madC^2), using the SCALED MAD on both sides. Quadrature is
// only justified because the inputs are sigma-equivalents; callers must not
// pass WanderMadRawNs here.
func PairwiseBand(madScaledA, madScaledC float64) float64 {
	return math.Sqrt(madScaledA*madScaledA + madScaledC*madScaledC)
}

// ─────────────────────────────────────────────────────────────────────────────
// Gates (wander-band-design.md §5). All reject-only: the band describes
// clock behaviour, it never corrects a sample. Each gate is a pure function
// returning (ok bool, reason string) - reason is populated only on failure,
// so a caller can log/emit it without a separate message-formatting step.
// ─────────────────────────────────────────────────────────────────────────────

// ExpectedSamples is hz*seconds - the sampler's own target sample count for
// a window, needed by GateSampleCoverage (G1) to compute the 90% floor.
//
// CRITICAL: seconds here must be the sampler's ACTUAL elapsed sampling
// window, not the maximum lifetime StartWander bounded it to. wanderSeconds
// (wander-sampler-lifecycle-design.md §3: timeout_sec+10, plus StartWander's
// own +30s margin) is deliberately generous - it exists to guarantee the
// child process is eventually reaped even if the caller never sends
// wander_stop, not to predict how long the sampler will actually run before
// StopWander cuts it off. StopWander fires right after the traffic run
// completes, which on this fleet is typically 1-10s - far short of a 70s+
// upper bound. Passing the upper bound here made G1 compare a ~15-sample
// real capture against an ~700-sample expectation and reject EVERY live
// campaign run regardless of how clean the data actually was. Found live on
// EC2 (ap-northeast-1, 3x m8a.2xlarge): every real campaign's wander data
// was rejected by G1 until this was corrected to use ActualDurationSec.
// Callers must derive seconds from the actual sampling window (see
// ActualDurationSec) or from the settle+run phase timers, never from the
// wanderSeconds upper bound passed to CmdWanderStart.
func ExpectedSamples(hz, seconds float64) int {
	return int(hz * seconds)
}

// ActualDurationSec returns the real elapsed span the sampler actually
// covered, from the first and last sample's own wall_ns timestamps -
// authoritative regardless of dispatch/network overhead on either side of
// the actual sampling, and the correct basis for ExpectedSamples/G1 (see
// that function's doc comment for why the theoretical upper-bound lifetime
// must not be used instead). Returns 0 for fewer than 2 samples, since a
// duration needs two points; GateSampleCoverage's own expectedSamples<=0
// guard then correctly fails closed rather than dividing by a meaningless
// duration.
func ActualDurationSec(samples []Sample) float64 {
	if len(samples) < 2 {
		return 0
	}
	first, last := samples[0].WallNs, samples[len(samples)-1].WallNs
	for _, s := range samples {
		if s.WallNs < first {
			first = s.WallNs
		}
		if s.WallNs > last {
			last = s.WallNs
		}
	}
	return float64(last-first) / 1e9
}

// GateSampleCoverage is G1: the sampler must have run for (at least 90% of)
// the whole window, or there is no clock evidence for part of the run.
func GateSampleCoverage(f Fields, expectedSamples int) (bool, string) {
	if expectedSamples <= 0 {
		return false, "G1: expectedSamples must be > 0 to evaluate coverage"
	}
	threshold := 0.9 * float64(expectedSamples)
	if float64(f.SampleCount) < threshold {
		return false, fmt.Sprintf("G1: sample_count %d < 90%% of expected %d (no clock evidence for part of the run)",
			f.SampleCount, expectedSamples)
	}
	return true, ""
}

// GateSentinelRate is G2: ioctl failures (sentinel rows) must not dominate
// the window, or device contention corrupted the evidence.
func GateSentinelRate(f Fields) (bool, string) {
	if f.SampleCount == 0 {
		return false, "G2: sample_count is 0, cannot evaluate sentinel rate"
	}
	pct := 100.0 * float64(f.SentinelCount) / float64(f.SampleCount)
	if pct >= 5.0 {
		return false, fmt.Sprintf("G2: sentinel_count %.2f%% >= 5%% (device contention corrupted the evidence)", pct)
	}
	return true, ""
}

// GateNoStep is G3: any clock step during the campaign invalidates every
// sample straddling it - not just the samples nearest the step - so this is
// a hard reject on a nonzero step count, not a rate threshold like G1/G2/G4.
func GateNoStep(f Fields) (bool, string) {
	if f.WanderStepCount != 0 {
		return false, fmt.Sprintf("G3: wander_step_count = %d (a clock step mid-run invalidates every sample straddling it)",
			f.WanderStepCount)
	}
	return true, ""
}

// PhcCounterSnapshot is one side (window start or window end) of G4's
// bracket - the counter values the agent's Runner.PhcCounters() returns.
// A nil map means "the agent could not read the counters" and is treated as
// unable to verify, distinct from "read and all zero".
type PhcCounterSnapshot map[string]int64

// phcMustNotMoveKeys are the counters G4 requires to be UNCHANGED across the
// window (wander-band-design.md §5: "phc_err_ts, phc_err_eb, phc_skp
// unchanged"). A change in any of these means the device hit its 125/s
// get-time cap or the block period it triggers during this exact window,
// which corrupts the wander evidence taken during it - not merely degrades
// its precision.
var phcMustNotMoveKeys = []string{"phc_err_ts", "phc_err_eb", "phc_skp"}

// GatePhcCountersClean is G4: phc_err_ts/phc_err_eb/phc_skp must not move
// during the window, and phc_exp/phc_cnt's implied error ratio must stay
// under 1% (vendor guidance, precision-opus5-high-design.md V-21). Absent
// counters on EITHER snapshot (agent could not read them) fail the gate as
// "cannot verify" rather than passing it - an older fleet node without this
// capability must never silently get a free pass on a gate meant to catch
// exactly the kind of corruption that node cannot report.
func GatePhcCountersClean(start, end PhcCounterSnapshot) (bool, string) {
	if start == nil || end == nil {
		return false, "G4: PHC counters unavailable on this node (cannot verify)"
	}
	for _, key := range phcMustNotMoveKeys {
		sv, sok := start[key]
		ev, eok := end[key]
		if !sok || !eok {
			return false, fmt.Sprintf("G4: counter %q missing from a snapshot (cannot verify)", key)
		}
		if sv != ev {
			return false, fmt.Sprintf("G4: counter %q moved during the window (%d -> %d) - "+
				"device hit its get-time rate cap or block period", key, sv, ev)
		}
	}
	exp, expOk := end["phc_exp"]
	cnt, cntOk := end["phc_cnt"]
	if !expOk || !cntOk {
		return false, "G4: phc_exp/phc_cnt missing from the end snapshot (cannot verify error ratio)"
	}
	if cnt == 0 {
		// No successful reads at all to form a ratio against - GateSampleCoverage
		// (G1) is what should have already rejected this window; G4 does not
		// need to double-fail it with a divide-by-zero special case, but it
		// must not silently pass either.
		return false, "G4: phc_cnt is 0, cannot compute an error ratio"
	}
	ratio := 100.0 * float64(exp) / float64(cnt)
	if ratio >= 1.0 {
		return false, fmt.Sprintf("G4: phc_exp/phc_cnt error ratio %.2f%% >= 1%% - reduce hz", ratio)
	}
	return true, ""
}

// GateRefclockIsPhc is G5: exactly one clock source must be unambiguously
// selected, and at most one PHC refclock line may be present (the
// duplicate-refclock config defect's signature - see
// Runner.parseRefclockSelected's doc comment in agent/runner.go). The name
// is kept for continuity with wander-band-design.md's original spec and the
// wire field (proto.CommandResult.RefclockPhcSelected), even though the
// check no longer requires the PHC specifically: configure_mcast.yaml
// deliberately prefers NTP over the PHC for one-way mcast accuracy, and a
// campaign run through that playbook has NTP as its correct, intended
// state - the original PHC-only version of this gate rejected every such
// run for a config choice that was working exactly as designed. selected/ok
// mirror Runner.RefclockSelected()'s return shape: ok=false means the agent
// could not answer the check at all (cannot verify).
func GateRefclockIsPhc(selected, ok bool) (bool, string) {
	if !ok {
		return false, "G5: could not read chronyc sources (cannot verify refclock selection)"
	}
	if !selected {
		return false, "G5: no single clock source is unambiguously selected (either nothing " +
			"converged, or more than one PHC refclock line is present - a different measurement " +
			"entirely, or an ambiguous one)"
	}
	return true, ""
}

// excursionFleetBaselinePct is the ~21% fleet baseline wander-band-design.md
// §1/§6 (G6) reports for wander_excursion_pct on this fleet - G6 flags a
// campaign whose excursion rate is FAR ABOVE this, as a chrony-health signal.
const excursionFleetBaselinePct = 21.0

// excursionFlagMultiplier is G6's "far above" threshold, expressed as a
// multiple of excursionFleetBaselinePct. The design doc gives the baseline
// figure but not a specific multiplier for "far above" - 2x (42%) is chosen
// as a deliberately loose bar: G6 is explicitly NOT a reject gate (§5: "Flag,
// do not reject"), so it should only fire on a genuinely abnormal excursion
// rate, not on ordinary fleet-to-fleet variance around the ~21% baseline.
const excursionFlagMultiplier = 2.0

// GateExcursionNorm is G6: NOT a reject gate. Returns (flag bool, reason
// string) - flag=true means "investigate chronyd health", and the caller is
// responsible for surfacing this as a warning, never for rejecting the run
// on it. This is the one gate in §5 marked "Flag, do not reject".
func GateExcursionNorm(f Fields) (bool, string) {
	threshold := excursionFleetBaselinePct * excursionFlagMultiplier
	if f.WanderExcursionPct > threshold {
		return true, fmt.Sprintf("G6: wander_excursion_pct %.2f%% far above the ~%.0f%% fleet baseline (investigate chronyd health)",
			f.WanderExcursionPct, excursionFleetBaselinePct)
	}
	return false, ""
}
