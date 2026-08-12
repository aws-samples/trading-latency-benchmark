package store

import (
	"testing"

	"afxdp-cp/backend/pairs"
	"afxdp-cp/proto"
)

// A campaign must be a queryable object, not just a timestamp range. Without a
// runs row and a run_id on each measurement, "compare the full mesh at 01:30
// with the one at 02:00" degrades into guessing at a time window that may
// straddle two campaigns.
func TestRunLineageLinksMeasurementsToRun(t *testing.T) {
	st := openTestStore(t)

	runID, err := st.InsertRun("ucast", "kernel", "among", `["i-1","i-2"]`, 2,
		map[string]any{"count": 5000})
	if err != nil {
		t.Fatalf("InsertRun: %v", err)
	}
	if runID <= 0 {
		t.Fatalf("InsertRun returned id %d, want > 0", runID)
	}

	// The store must tag incoming telemetry with the campaign that is running.
	st.SetCurrentRun(runID)
	if got := st.CurrentRun(); got != runID {
		t.Fatalf("CurrentRun = %d, want %d", got, runID)
	}

	tel := proto.Telemetry{
		Kind: "ucast", Variation: "kernel", SrcIP: "10.0.0.1", DstIP: "10.0.0.2",
		Unix: 1000, Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 33, P99: 40}},
	}
	RecordMeasurement(st, tel, st.CurrentRun())
	st.Flush()

	var gotRun int64
	err = st.db.QueryRow(`SELECT run_id FROM measurements WHERE src_ip='10.0.0.1'`).Scan(&gotRun)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if gotRun != runID {
		t.Fatalf("measurement run_id = %d, want %d", gotRun, runID)
	}

	// Finishing the run records how many pairs actually produced numbers, which
	// is what makes a partially-failed campaign distinguishable from a clean one.
	st.FinishRun(runID, 2)
	var endedAt, pairsOK, pairsTotal int64
	var scope, targets string
	err = st.db.QueryRow(
		`SELECT ended_at, pairs_ok, pairs_total, scope, target_ids FROM runs WHERE id=?`,
		runID).Scan(&endedAt, &pairsOK, &pairsTotal, &scope, &targets)
	if err != nil {
		t.Fatalf("read run: %v", err)
	}
	if endedAt == 0 {
		t.Fatal("ended_at not set by FinishRun")
	}
	if pairsOK != 2 || pairsTotal != 2 {
		t.Fatalf("pairs_ok/total = %d/%d, want 2/2", pairsOK, pairsTotal)
	}
	if scope != "among" || targets != `["i-1","i-2"]` {
		t.Fatalf("scope/targets = %q/%q", scope, targets)
	}

	// Clearing means later stray telemetry is not misattributed to the campaign.
	st.SetCurrentRun(0)
	if got := st.CurrentRun(); got != 0 {
		t.Fatalf("CurrentRun after clear = %d, want 0", got)
	}
}

// The whole persistence layer is optional, so every lineage entry point has to
// tolerate a nil Store. A panic here would take the backend down when a user
// simply passed --db-path="".
func TestRunLineageNilStoreIsSafe(t *testing.T) {
	var st *Store
	// None of these may panic.
	st.SetCurrentRun(7)
	if got := st.CurrentRun(); got != 0 {
		t.Fatalf("nil-store CurrentRun = %d, want 0", got)
	}
	if _, err := st.InsertRun("ucast", "kernel", "among", "", 0, nil); err != nil {
		t.Fatalf("nil-store InsertRun should be a silent no-op, got %v", err)
	}
	st.FinishRun(1, 1)
	RecordMeasurement(st, proto.Telemetry{Kind: "ucast"}, 0)
	st.Flush()
}

// A measurement that arrives outside any campaign (a manual agent run) must
// still be stored, with a NULL run_id rather than a dangling reference.
func TestMeasurementWithoutRunIsStoredUnlinked(t *testing.T) {
	st := openTestStore(t)

	RecordMeasurement(st, proto.Telemetry{
		Kind: "ucast", Variation: "kernel", SrcIP: "10.0.0.9", DstIP: "10.0.0.8", Unix: 500,
	}, 0)
	st.Flush()

	var runID any
	err := st.db.QueryRow(`SELECT run_id FROM measurements WHERE src_ip='10.0.0.9'`).Scan(&runID)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if runID != nil {
		t.Fatalf("run_id = %v, want NULL for an unlinked measurement", runID)
	}
}

// scopeName is what lands in runs.scope. A full mesh must be distinguishable
// from an "among" run that happened to cover everything, otherwise a later
// comparison cannot tell a whole-fleet sweep from a lucky subset.
func TestScopeNameDistinguishesFullMesh(t *testing.T) {
	cases := []struct {
		scope string
		k     int
		want  string
	}{
		{"", 0, "full"},
		{"among", 0, "full"},
		{"fanout", 0, "full"},
		{"", 2, "among"},
		{"among", 2, "among"},
		{"fanout", 1, "fanout"},
		{"fanin", 3, "fanin"},
	}
	for _, tc := range cases {
		if got := pairs.ScopeName(tc.scope, tc.k); got != tc.want {
			t.Fatalf("ScopeName(%q, %d) = %q, want %q", tc.scope, tc.k, got, tc.want)
		}
	}
}

// The point of the runs table is that "the campaign at 01:30" becomes an object
// you can query, rather than a time window that might straddle two campaigns.
// This asserts that property directly: two overlapping-in-time campaigns must be
// separable by run_id.
func TestTwoCampaignsAreSeparableByRunID(t *testing.T) {
	st := openTestStore(t)

	runA, _ := st.InsertRun("ucast", "kernel", "full", "", 2, nil)
	st.SetCurrentRun(runA)
	RecordMeasurement(st, proto.Telemetry{Kind: "ucast", Variation: "kernel",
		SrcIP: "10.0.0.1", DstIP: "10.0.0.2", Unix: 100,
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 30}}}, st.CurrentRun())
	st.FinishRun(runA, 1)

	runB, _ := st.InsertRun("ucast", "xdp", "among", `["i-1","i-2"]`, 2, nil)
	st.SetCurrentRun(runB)
	// Same edge, same second: only run_id separates these two samples.
	RecordMeasurement(st, proto.Telemetry{Kind: "ucast", Variation: "xdp",
		SrcIP: "10.0.0.1", DstIP: "10.0.0.2", Unix: 100,
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 28}}}, st.CurrentRun())
	st.FinishRun(runB, 1)
	st.SetCurrentRun(0)
	st.Flush()

	if runA == runB || runA <= 0 || runB <= 0 {
		t.Fatalf("run ids must be distinct and positive, got %d and %d", runA, runB)
	}
	for _, tc := range []struct{ run, wantP50 int64 }{{runA, 30}, {runB, 28}} {
		var p50 int64
		if err := st.db.QueryRow(
			`SELECT p50 FROM measurements WHERE run_id=? AND src_ip='10.0.0.1'`, tc.run,
		).Scan(&p50); err != nil {
			t.Fatalf("query run %d: %v", tc.run, err)
		}
		if p50 != tc.wantP50 {
			t.Fatalf("run %d p50 = %d, want %d", tc.run, p50, tc.wantP50)
		}
	}
	// And the runs themselves carry the scope that produced them.
	var scope string
	if err := st.db.QueryRow(`SELECT scope FROM runs WHERE id=?`, runB).Scan(&scope); err != nil {
		t.Fatalf("query run scope: %v", err)
	}
	if scope != "among" {
		t.Fatalf("run B scope = %q, want among", scope)
	}
}
