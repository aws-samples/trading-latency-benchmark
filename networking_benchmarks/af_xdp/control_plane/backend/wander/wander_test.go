package wander

import (
	"math"
	"strings"
	"testing"
)

func almostEqual(a, b, tol float64) bool { return math.Abs(a-b) <= tol }

// TestReduceMadScalingGoldenFile guards the exact bug wander-band-design.md
// §4 calls out by name: "analyze.py reports its mad column SCALED by
// 1.4826... An implementation that computes raw MAD and compares against
// those published numbers will under-report the band by a factor of 1.48."
// The doc's own worked re-derivation ("destination phc-sys raw MAD 1349.0 ->
// x1.4826 = 2000.0 against a published 2000") is reproduced here exactly:
// a dataset engineered so nearestRankPercentile's abs-deviations-from-median
// step lands on a raw MAD of 1349.0, and the scaled MAD must equal 2000.0
// (within floating-point tolerance, not by hardcoding the multiply).
func TestReduceMadScalingGoldenFile(t *testing.T) {
	// Nine points with median exactly 0 (the middle sorted value), chosen so
	// the nearest-rank median of |offset - median| lands exactly on 1349:
	// sorted offsets: -3000,-2200,-1800,-1349,0,100,600,900,1600 -> median=0
	// sorted abs-devs: 0,100,600,900,1349,1600,1800,2200,3000 -> median (idx 4) = 1349.
	offsets := []float64{-3000, -2200, -1800, -1349, 0, 100, 600, 900, 1600}
	samples := offsetsToSamples(offsets)

	f := Reduce(samples)
	const wantRaw = 1349.0
	const wantScaled = 2000.0 // 1349.0 * 1.4826, matching the doc's own worked figure

	if !almostEqual(f.WanderMadRawNs, wantRaw, 0.5) {
		t.Fatalf("WanderMadRawNs = %.4f, want %.1f (median of abs deviations)", f.WanderMadRawNs, wantRaw)
	}
	if !almostEqual(f.WanderMadScaledNs, wantScaled, 1.0) {
		t.Fatalf("WanderMadScaledNs = %.4f, want %.1f (raw x 1.4826, matching FINDINGS.md's published 2000)",
			f.WanderMadScaledNs, wantScaled)
	}
	// The trap itself: scaled must be raw x 1.4826, not raw itself.
	if almostEqual(f.WanderMadScaledNs, f.WanderMadRawNs, 100) {
		t.Fatal("WanderMadScaledNs must not equal WanderMadRawNs - this IS the under-report-by-1.48x bug the doc warns about")
	}
}

// TestReduceMadPerSqrtSec checks the duration-normalized field directly:
// WanderMadPerSqrtSecNs must equal WanderMadScaledNs / sqrt(WanderDurationSec),
// using samples spaced so ActualDurationSec is a known, exact value.
func TestReduceMadPerSqrtSec(t *testing.T) {
	// 5 samples at 1-second spacing (wall_ns), spanning exactly 4 seconds.
	offsets := []float64{-2000, -1000, 0, 1000, 2000}
	samples := make([]Sample, len(offsets))
	for i, o := range offsets {
		samples[i] = Sample{Seq: int64(i), WallNs: int64(i) * 1_000_000_000, OffsetNs: int64(o), BracketNs: 1000, EbNs: 20000}
	}
	f := Reduce(samples)

	if !almostEqual(f.WanderDurationSec, 4.0, 0.001) {
		t.Fatalf("WanderDurationSec = %.4f, want 4.0 (5 samples at 1s spacing spans 4s)", f.WanderDurationSec)
	}
	wantNorm := f.WanderMadScaledNs / math.Sqrt(4.0)
	if !almostEqual(f.WanderMadPerSqrtSecNs, wantNorm, 0.5) {
		t.Fatalf("WanderMadPerSqrtSecNs = %.4f, want %.4f (WanderMadScaledNs / sqrt(WanderDurationSec))",
			f.WanderMadPerSqrtSecNs, wantNorm)
	}
}

// TestReduceMadPerSqrtSecMatchesLiveDataShrinkage reproduces, at reduced
// scale, an effect where two runs have very different apparent MAD_scaled,
// but where most of the difference is explained by one run's window being
// far shorter than the other's - duration normalization must narrow the
// ratio between them, not leave it unchanged and not invert it.
func TestReduceMadPerSqrtSecMatchesLiveDataShrinkage(t *testing.T) {
	shortRun := offsetsToSamplesWithSpacing([]float64{-200, -100, 0, 100, 200}, 200_000_000) // 5 samples, 0.2s apart -> 0.8s span
	longRun := offsetsToSamplesWithSpacing([]float64{-2000, -1000, 0, 1000, 2000}, 2_000_000_000) // 5 samples, 2s apart -> 8s span

	fShort := Reduce(shortRun)
	fLong := Reduce(longRun)

	rawRatio := fLong.WanderMadScaledNs / fShort.WanderMadScaledNs
	normRatio := fLong.WanderMadPerSqrtSecNs / fShort.WanderMadPerSqrtSecNs

	if normRatio >= rawRatio {
		t.Fatalf("duration normalization must narrow the apparent spread: raw ratio=%.2f, normalized ratio=%.2f (normalized must be smaller)",
			rawRatio, normRatio)
	}
}

// TestReduceMadPerSqrtSecZeroDuration: fewer than 2 real samples means
// ActualDurationSec returns 0 (no span to measure) - WanderMadPerSqrtSecNs
// must then be left at its zero value, never divide by zero.
func TestReduceMadPerSqrtSecZeroDuration(t *testing.T) {
	samples := []Sample{{Seq: 0, WallNs: 100, OffsetNs: 500, BracketNs: 1000, EbNs: 20000}}
	f := Reduce(samples)
	if f.WanderDurationSec != 0 {
		t.Fatalf("WanderDurationSec = %.4f, want 0 (single sample has no span)", f.WanderDurationSec)
	}
	if f.WanderMadPerSqrtSecNs != 0 {
		t.Fatalf("WanderMadPerSqrtSecNs = %.4f, want 0 (must not divide by zero duration)", f.WanderMadPerSqrtSecNs)
	}
}

// TestReduceMedianIsZeroMean checks wander-band-design.md §1's stated
// property directly: a symmetric, zero-mean offset distribution must reduce
// to a median of (approximately) zero, not be pulled off-center the way a
// mean would be by a one-sided tail (the doc's own "609 ns node-specific
// skew that was a mean over a one-sided tail while the median was -15 ns"
// cautionary example, §4 "Never mean/sd").
func TestReduceMedianIsZeroMean(t *testing.T) {
	// One-sided tail: mostly small values near zero, one large outlier.
	// A mean would be pulled toward the outlier; the median must not be.
	offsets := []float64{-10, -5, 0, 5, 10, 15, 20000}
	f := Reduce(offsetsToSamples(offsets))
	if math.Abs(f.WanderMedNs) > 20 {
		t.Fatalf("median pulled toward outlier: got %.1f, want near 0 (this is the mean-vs-median trap the doc warns about)", f.WanderMedNs)
	}
}

// TestReduceExcursionPct must match wander-band-design.md §4's definition
// exactly: share of samples with |offset| > 3us (3000ns), reported as a
// percentage of NON-sentinel samples.
func TestReduceExcursionPct(t *testing.T) {
	// 10 samples, 3 exceed the 3000ns threshold in absolute value.
	offsets := []float64{0, 100, -100, 3001, -3500, 500, -500, 10000, 200, -200}
	f := Reduce(offsetsToSamples(offsets))
	want := 30.0 // 3/10
	if !almostEqual(f.WanderExcursionPct, want, 0.01) {
		t.Fatalf("WanderExcursionPct = %.2f, want %.2f", f.WanderExcursionPct, want)
	}
}

// TestReduceExcursionBoundary: exactly 3000ns must NOT count (threshold is
// "> 3us", not ">="), per the doc's own wording.
func TestReduceExcursionBoundary(t *testing.T) {
	offsets := []float64{3000, -3000, 3000.0}
	f := Reduce(offsetsToSamples(offsets))
	if f.WanderExcursionPct != 0 {
		t.Fatalf("exactly-3000ns samples must not count as excursions (threshold is strictly >), got %.2f%%", f.WanderExcursionPct)
	}
}

// TestReduceStepCount must detect a genuine discontinuity between
// consecutive samples (a clock step, G3's trigger) and must NOT flag
// ordinary wander that stays within the design doc's own observed range
// (+/-4.7us p99 excursions, per §1 - well under the 10us step threshold).
func TestReduceStepCount(t *testing.T) {
	// Ordinary wander: small deltas throughout, nothing exceeds 10000ns
	// between consecutive samples.
	normal := []float64{0, 500, -300, 800, -600, 400, -900, 1000}
	if got := Reduce(offsetsToSamples(normal)).WanderStepCount; got != 0 {
		t.Fatalf("ordinary wander incorrectly flagged as a step: count=%d", got)
	}

	// A genuine step: offset jumps by 50000ns between two consecutive samples.
	stepped := []float64{0, 100, 200, 50300, 50200, 50100}
	if got := Reduce(offsetsToSamples(stepped)).WanderStepCount; got != 1 {
		t.Fatalf("genuine 50000ns step not detected: count=%d, want 1", got)
	}
}

// TestReduceStepCountIgnoresSentinelGaps: a sentinel row (ioctl failure) must
// not itself be counted as a step, and must not be treated as if it were a
// real offset of -1 when forming the delta across the gap it creates.
func TestReduceStepCountIgnoresSentinelGaps(t *testing.T) {
	samples := []Sample{
		{Seq: 0, OffsetNs: 100, BracketNs: 1000, EbNs: 20000},
		{Seq: 1, Sentinel: true, PhcNs: -1, OffsetNs: -1, BracketNs: -1, EbNs: -1},
		{Seq: 2, OffsetNs: 150, BracketNs: 1000, EbNs: 20000}, // small real delta vs seq 0, not vs the sentinel's -1
	}
	f := Reduce(samples)
	if f.WanderStepCount != 0 {
		t.Fatalf("sentinel gap incorrectly produced a step: count=%d (delta must be computed between the two REAL samples, 100->150, not against the sentinel's -1)", f.WanderStepCount)
	}
	if f.SentinelCount != 1 {
		t.Fatalf("SentinelCount = %d, want 1", f.SentinelCount)
	}
	if f.SampleCount != 3 {
		t.Fatalf("SampleCount = %d, want 3 (total rows including sentinel)", f.SampleCount)
	}
}

// TestReduceEmptyInput must return a zero Fields, not panic or divide by
// zero - the design doc's own gates (G1/G2) are what should reject an
// under-sampled run; Reduce itself must be total.
func TestReduceEmptyInput(t *testing.T) {
	f := Reduce(nil)
	if f.SampleCount != 0 || f.WanderMedNs != 0 || f.WanderMadScaledNs != 0 {
		t.Fatalf("empty input must reduce to a zero Fields, got %+v", f)
	}
}

// TestReduceAllSentinel: every row is a sentinel (device never answered) -
// must not panic, and must report SampleCount/SentinelCount correctly with
// every statistical field left at zero.
func TestReduceAllSentinel(t *testing.T) {
	samples := []Sample{
		{Seq: 0, Sentinel: true, PhcNs: -1, OffsetNs: -1, BracketNs: -1, EbNs: -1},
		{Seq: 1, Sentinel: true, PhcNs: -1, OffsetNs: -1, BracketNs: -1, EbNs: -1},
	}
	f := Reduce(samples)
	if f.SampleCount != 2 || f.SentinelCount != 2 {
		t.Fatalf("counts wrong: %+v", f)
	}
	if f.WanderMedNs != 0 || f.WanderMadScaledNs != 0 {
		t.Fatalf("all-sentinel input must not produce nonzero statistics: %+v", f)
	}
}

// TestNearestRankPercentileNeverInterpolates matches the reference
// implementation's contract: every returned value must be a value that was
// actually present in the input, never an interpolated point between two
// samples.
func TestNearestRankPercentileNeverInterpolates(t *testing.T) {
	xs := []float64{10, 20, 30, 40}
	present := map[float64]bool{10: true, 20: true, 30: true, 40: true}
	for _, q := range []float64{0, 1, 25, 50, 75, 99, 100} {
		got := nearestRankPercentile(xs, q)
		if !present[got] {
			t.Fatalf("nearestRankPercentile(xs, %.0f) = %v, not a value present in xs (must never interpolate)", q, got)
		}
	}
}

// TestPairwiseBandQuadrature matches wander-band-design.md §4's worked
// reference figures for the pooled reference battery: two per-node scaled
// MADs combining in quadrature to approximately (not exactly - the doc is
// explicit the fit is "conservative rather than exact", observed 90% of
// predicted) the documented predicted figure.
func TestPairwiseBandQuadrature(t *testing.T) {
	// Reference battery per-node MAD range from §1: "MAD 1302-1686 ns".
	// sqrt(1500^2+1500^2) = 2121.32, matching the doc's own "predicted
	// (sqrt(2) x per-node)" figure of 2121ns for the pooled reference battery.
	got := PairwiseBand(1500, 1500)
	want := 2121.32
	if !almostEqual(got, want, 1.0) {
		t.Fatalf("PairwiseBand(1500,1500) = %.2f, want ~%.2f (doc's own sqrt(2)x prediction)", got, want)
	}
}

// TestParseCSVSkipsHeaderAndMalformedRows checks the parser against
// phcsample's actual output shape, including its header line and its
// documented sentinel row format.
func TestParseCSVSkipsHeaderAndMalformedRows(t *testing.T) {
	csvText := `seq,wall_ns,phc_ns,sysmid_ns,offset_ns,bracket_ns,eb_ns
0,1000,2000,1500,500,1000,20000
1,2000,-1,-1,-1,-1,-1
garbage,line,that,should,be,skipped
2,3000,4000,3500,500,1000,20000
`
	samples, err := ParseCSV(strings.NewReader(csvText))
	if err != nil {
		t.Fatalf("ParseCSV: %v", err)
	}
	if len(samples) != 3 {
		t.Fatalf("want 3 parsed rows (header + malformed line skipped), got %d: %+v", len(samples), samples)
	}
	if samples[0].Sentinel || samples[2].Sentinel {
		t.Fatalf("rows 0 and 2 must not be sentinels: %+v", samples)
	}
	if !samples[1].Sentinel {
		t.Fatalf("row 1 (phc_ns=-1) must be classified as a sentinel: %+v", samples[1])
	}
}

// TestParseCSVEmptyInput must return an empty (not nil-panicking) slice for
// an empty or header-only input.
func TestParseCSVEmptyInput(t *testing.T) {
	samples, err := ParseCSV(strings.NewReader("seq,wall_ns,phc_ns,sysmid_ns,offset_ns,bracket_ns,eb_ns\n"))
	if err != nil {
		t.Fatalf("ParseCSV: %v", err)
	}
	if len(samples) != 0 {
		t.Fatalf("header-only input must parse to zero samples, got %d", len(samples))
	}
}

// offsetsToSamples builds a minimal Sample slice from a list of offsets,
// with BracketNs/EbNs held constant - sufficient for tests that only assert
// on offset-derived fields.
func offsetsToSamples(offsets []float64) []Sample {
	out := make([]Sample, len(offsets))
	for i, o := range offsets {
		out[i] = Sample{Seq: int64(i), OffsetNs: int64(o), BracketNs: 1000, EbNs: 20000}
	}
	return out
}

// offsetsToSamplesWithSpacing is offsetsToSamples plus an explicit WallNs
// spacing between consecutive samples, for tests that need ActualDurationSec
// (and therefore WanderMadPerSqrtSecNs) to be a known, non-zero value.
func offsetsToSamplesWithSpacing(offsets []float64, spacingNs int64) []Sample {
	out := make([]Sample, len(offsets))
	for i, o := range offsets {
		out[i] = Sample{Seq: int64(i), WallNs: int64(i) * spacingNs, OffsetNs: int64(o), BracketNs: 1000, EbNs: 20000}
	}
	return out
}

// ─────────────────────────────────────────────────────────────────────────────
// Gate tests (wander-band-design.md §5). Each gate gets a test that forces
// the failure AND a test that confirms a clean/passing case does not
// trigger it - "a gate without a failing test is not a gate" (§6, W4).
// ─────────────────────────────────────────────────────────────────────────────

// G1: sample_count below 90% of expected must reject; at/above must pass.
func TestGateSampleCoverage(t *testing.T) {
	if ok, reason := GateSampleCoverage(Fields{SampleCount: 100}, 200); ok {
		t.Fatalf("50%% coverage must fail G1, got ok=true reason=%q", reason)
	}
	if ok, _ := GateSampleCoverage(Fields{SampleCount: 180}, 200); !ok {
		t.Fatal("90% coverage (the threshold itself) must pass G1")
	}
	if ok, _ := GateSampleCoverage(Fields{SampleCount: 200}, 200); !ok {
		t.Fatal("full coverage must pass G1")
	}
	if ok, reason := GateSampleCoverage(Fields{SampleCount: 200}, 0); ok {
		t.Fatalf("expectedSamples<=0 must fail closed (cannot evaluate), got ok=true reason=%q", reason)
	}
}

// G2: sentinel rate at/above 5% must reject; below must pass.
func TestGateSentinelRate(t *testing.T) {
	if ok, reason := GateSentinelRate(Fields{SampleCount: 100, SentinelCount: 5}); ok {
		t.Fatalf("5%% sentinel rate must fail G2 (threshold is >=5%%), got ok=true reason=%q", reason)
	}
	if ok, _ := GateSentinelRate(Fields{SampleCount: 100, SentinelCount: 4}); !ok {
		t.Fatal("4% sentinel rate must pass G2")
	}
	if ok, _ := GateSentinelRate(Fields{SampleCount: 100, SentinelCount: 0}); !ok {
		t.Fatal("0% sentinel rate must pass G2")
	}
	if ok, reason := GateSentinelRate(Fields{SampleCount: 0}); ok {
		t.Fatalf("sample_count=0 must fail closed, got ok=true reason=%q", reason)
	}
}

// G3: any nonzero step count must reject - not a rate threshold like G1/G2.
func TestGateNoStep(t *testing.T) {
	if ok, reason := GateNoStep(Fields{WanderStepCount: 1}); ok {
		t.Fatalf("a single step must fail G3, got ok=true reason=%q", reason)
	}
	if ok, _ := GateNoStep(Fields{WanderStepCount: 0}); !ok {
		t.Fatal("zero steps must pass G3")
	}
}

// G4: any of phc_err_ts/phc_err_eb/phc_skp moving during the window must
// reject; all unchanged with a healthy error ratio must pass.
func TestGatePhcCountersClean(t *testing.T) {
	start := PhcCounterSnapshot{"phc_cnt": 100, "phc_exp": 0, "phc_skp": 0, "phc_err_dv": 0, "phc_err_ts": 0, "phc_err_eb": 0}

	// Clean: nothing moved, low error ratio.
	end := PhcCounterSnapshot{"phc_cnt": 200, "phc_exp": 0, "phc_skp": 0, "phc_err_dv": 0, "phc_err_ts": 0, "phc_err_eb": 0}
	if ok, reason := GatePhcCountersClean(start, end); !ok {
		t.Fatalf("clean window must pass G4, got reason=%q", reason)
	}

	// phc_skp moved - device hit its block period.
	movedSkp := PhcCounterSnapshot{"phc_cnt": 200, "phc_exp": 0, "phc_skp": 3, "phc_err_dv": 0, "phc_err_ts": 0, "phc_err_eb": 0}
	if ok, reason := GatePhcCountersClean(start, movedSkp); ok {
		t.Fatalf("phc_skp moving must fail G4, got ok=true reason=%q", reason)
	}

	// phc_err_ts moved.
	movedErrTs := PhcCounterSnapshot{"phc_cnt": 200, "phc_exp": 0, "phc_skp": 0, "phc_err_dv": 0, "phc_err_ts": 1, "phc_err_eb": 0}
	if ok, _ := GatePhcCountersClean(start, movedErrTs); ok {
		t.Fatal("phc_err_ts moving must fail G4")
	}

	// Error ratio >= 1%: phc_exp/phc_cnt.
	highRatio := PhcCounterSnapshot{"phc_cnt": 200, "phc_exp": 3, "phc_skp": 0, "phc_err_dv": 0, "phc_err_ts": 0, "phc_err_eb": 0}
	if ok, reason := GatePhcCountersClean(start, highRatio); ok {
		t.Fatalf("1.5%% error ratio (3/200) must fail G4 (threshold >=1%%), got ok=true reason=%q", reason)
	}

	// Missing counters on either side must fail closed, not pass.
	if ok, reason := GatePhcCountersClean(nil, end); ok {
		t.Fatalf("nil start snapshot must fail G4 (cannot verify), got ok=true reason=%q", reason)
	}
	if ok, reason := GatePhcCountersClean(start, nil); ok {
		t.Fatalf("nil end snapshot must fail G4 (cannot verify), got ok=true reason=%q", reason)
	}

	// phc_cnt=0 in the end snapshot must fail closed (no successful reads
	// to form a ratio against), not divide by zero or silently pass.
	zeroCnt := PhcCounterSnapshot{"phc_cnt": 0, "phc_exp": 0, "phc_skp": 0, "phc_err_dv": 0, "phc_err_ts": 0, "phc_err_eb": 0}
	startZero := PhcCounterSnapshot{"phc_cnt": 0, "phc_exp": 0, "phc_skp": 0, "phc_err_dv": 0, "phc_err_ts": 0, "phc_err_eb": 0}
	if ok, reason := GatePhcCountersClean(startZero, zeroCnt); ok {
		t.Fatalf("phc_cnt=0 must fail closed, got ok=true reason=%q", reason)
	}
}

// G5: refclock not selected, or the agent unable to answer at all, must
// reject; PHC selected must pass.
func TestGateRefclockIsPhc(t *testing.T) {
	if ok, reason := GateRefclockIsPhc(false, true); ok {
		t.Fatalf("refclock not selected must fail G5, got ok=true reason=%q", reason)
	}
	if ok, reason := GateRefclockIsPhc(false, false); ok {
		t.Fatalf("agent unable to answer (ok=false) must fail G5 as cannot-verify, got ok=true reason=%q", reason)
	}
	if ok, _ := GateRefclockIsPhc(true, true); !ok {
		t.Fatal("unambiguous single-source selection (PHC or NTP - see runner.go's parseRefclockSelected) must pass G5")
	}
}

// G6: NOT a reject gate. Must flag a genuinely abnormal excursion rate, and
// must NOT flag ordinary fleet variance around the ~21% baseline.
func TestGateExcursionNorm(t *testing.T) {
	if flag, reason := GateExcursionNorm(Fields{WanderExcursionPct: 21.0}); flag {
		t.Fatalf("the fleet baseline itself (21%%) must not be flagged, got flag=true reason=%q", reason)
	}
	if flag, reason := GateExcursionNorm(Fields{WanderExcursionPct: 30.0}); flag {
		t.Fatalf("ordinary variance above baseline (30%%) must not be flagged, got flag=true reason=%q", reason)
	}
	if flag, reason := GateExcursionNorm(Fields{WanderExcursionPct: 50.0}); !flag {
		t.Fatalf("50%% excursion rate (far above the ~21%% baseline) must be flagged, got flag=false reason=%q", reason)
	}
}

// ExpectedSamples must compute hz*seconds directly - the arithmetic G1's
// caller uses to derive its own threshold input.
func TestExpectedSamples(t *testing.T) {
	if got := ExpectedSamples(10, 90); got != 900 {
		t.Fatalf("ExpectedSamples(10, 90) = %d, want 900", got)
	}
}

// ActualDurationSec must derive the real elapsed window from the samples'
// own wall_ns span, NOT from any theoretical upper-bound lifetime - this is
// the exact fix for a bug found live on EC2 (wander.go's ExpectedSamples
// doc comment): G1 was comparing a real ~15-sample capture against a
// ~700-sample expectation derived from the sampler's 70s+ maximum lifetime,
// rejecting every live campaign regardless of actual data quality.
func TestActualDurationSec(t *testing.T) {
	// 16 samples at 10Hz (100ms apart) spans 1.5s - matching the real EC2
	// campaign that exposed this bug (a ~1.5s mcast run, wanderSeconds=70).
	samples := make([]Sample, 16)
	for i := range samples {
		samples[i] = Sample{WallNs: int64(i) * 100_000_000, OffsetNs: 100}
	}
	got := ActualDurationSec(samples)
	want := 1.5
	if !almostEqual(got, want, 0.01) {
		t.Fatalf("ActualDurationSec = %.3f, want %.3f", got, want)
	}
}

// ActualDurationSec must return 0 for fewer than 2 samples (no span to
// measure), and GateSampleCoverage must then fail closed via its own
// expectedSamples<=0 guard rather than accepting a meaningless duration.
func TestActualDurationSecInsufficientSamples(t *testing.T) {
	if got := ActualDurationSec(nil); got != 0 {
		t.Fatalf("ActualDurationSec(nil) = %.3f, want 0", got)
	}
	if got := ActualDurationSec([]Sample{{WallNs: 100}}); got != 0 {
		t.Fatalf("ActualDurationSec with 1 sample = %.3f, want 0", got)
	}
}

// The exact scenario found live: G1 must PASS a genuinely well-sampled
// short window (16 samples over 1.5s at 10Hz is ~107% coverage of the
// window it actually spans) when evaluated against ActualDurationSec, even
// though it would have failed against a 70s theoretical upper bound.
func TestGateSampleCoverageUsesActualNotTheoreticalDuration(t *testing.T) {
	samples := make([]Sample, 16)
	for i := range samples {
		samples[i] = Sample{WallNs: int64(i) * 100_000_000, OffsetNs: 100}
	}
	f := Reduce(samples)

	// WRONG basis (what the live bug did): compare against a 70s upper bound.
	wrongExpected := ExpectedSamples(10, 70)
	if ok, _ := GateSampleCoverage(f, wrongExpected); ok {
		t.Fatal("sanity check failed: 16 samples must not appear to cover a 700-sample/70s expectation")
	}

	// RIGHT basis (the fix): compare against the actual sampled span.
	rightExpected := ExpectedSamples(10, ActualDurationSec(samples))
	if ok, reason := GateSampleCoverage(f, rightExpected); !ok {
		t.Fatalf("16 samples over their own actual 1.5s span must pass G1, got reason=%q", reason)
	}
}
