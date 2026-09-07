package store

import (
	"database/sql"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"

	"afxdp-cp/backend/collector"
	"afxdp-cp/proto"
)

func openTestStore(t *testing.T) *Store {
	t.Helper()
	dir := t.TempDir()
	s, err := OpenStore(filepath.Join(dir, "test.db"), 7)
	if err != nil {
		t.Fatalf("OpenStore: %v", err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

// (a) Insert then read back.
func TestStoreInsertAndRead(t *testing.T) {
	s := openTestStore(t)

	runID, err := s.InsertRun("ucast", "kernel", "among", `["i-1","i-2"]`, 2, nil)
	if err != nil {
		t.Fatalf("InsertRun: %v", err)
	}
	if runID <= 0 {
		t.Fatalf("expected positive run_id, got %d", runID)
	}

	row := measurementRow{
		RunID:     runID,
		Unix:      time.Now().Unix(),
		Kind:      "ucast",
		Variation: "kernel",
		SrcIP:     "10.0.0.1",
		DstIP:     "10.0.0.2",
		TxMode:    "zero-copy",
		P50:       32, P90: 34, P99: 37, P999: 45, Max: 60, Min: 28, Mean: 33,
		Messages: 100000, Lost: 0, LossPct: 0.0,
		CmdID: "cmd-1",
	}
	s.ch <- row
	// Give the writer time to flush.
	s.Flush()

	var count int
	err = s.db.QueryRow("SELECT COUNT(*) FROM measurements WHERE src_ip=? AND dst_ip=?",
		"10.0.0.1", "10.0.0.2").Scan(&count)
	if err != nil {
		t.Fatalf("query: %v", err)
	}
	if count != 1 {
		t.Fatalf("expected 1 row, got %d", count)
	}

	var p50 int64
	err = s.db.QueryRow("SELECT p50 FROM measurements WHERE src_ip='10.0.0.1' AND dst_ip='10.0.0.2'").Scan(&p50)
	if err != nil {
		t.Fatalf("query p50: %v", err)
	}
	if p50 != 32 {
		t.Fatalf("expected p50=32, got %d", p50)
	}
}

// (b) SeedCollector repopulates rings chronologically with newest last.
func TestStoreSeedCollector(t *testing.T) {
	s := openTestStore(t)

	// Insert 5 measurements for one edge, out of chronological order in DB,
	// but SeedCollector must replay in ASC order and leave newest last.
	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 1, nil)
	base := int64(1700000000)
	for i := 0; i < 5; i++ {
		_, err := s.db.Exec(
			`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct)
			 VALUES (?, ?, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', ?, 34, 37, 45, 60, 28, 33, 100, 0, 0.0)`,
			runID, base+int64(i), int64(100+i))
		if err != nil {
			t.Fatalf("insert %d: %v", i, err)
		}
	}

	c := collector.NewCollector()
	if err := s.SeedCollector(c, 60); err != nil {
		t.Fatalf("SeedCollector: %v", err)
	}

	snap := c.Snapshot()
	if len(snap) != 1 {
		t.Fatalf("expected 1 edge, got %d", len(snap))
	}
	e := snap[0]
	// Newest sample has p50=104 (i=4, base+4).
	if e.Metrics.ServiceRTT.P50 != 104 {
		t.Fatalf("expected newest p50=104, got %d", e.Metrics.ServiceRTT.P50)
	}
	if e.Unix != base+4 {
		t.Fatalf("expected unix=%d, got %d", base+4, e.Unix)
	}
	// History should be [100,101,102,103,104] - chronological.
	if len(e.History) != 5 {
		t.Fatalf("expected 5 history entries, got %d", len(e.History))
	}
	if e.History[0].P50 != 100 || e.History[4].P50 != 104 {
		t.Fatalf("history not in chronological order: %v", e.History)
	}
}

// (c) Retention deletes only rows older than the window.
func TestStoreRetention(t *testing.T) {
	s := openTestStore(t)

	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 2, nil)
	now := time.Now().Unix()
	old := now - 8*24*3600 // 8 days ago (outside 7-day window)
	recent := now - 3600   // 1 hour ago (inside window)

	for _, ts := range []int64{old, recent} {
		_, err := s.db.Exec(
			`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, messages, lost, loss_pct)
			 VALUES (?, ?, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', 32, 100, 0, 0.0)`,
			runID, ts)
		if err != nil {
			t.Fatalf("insert: %v", err)
		}
	}

	s.runRetention(7)

	var count int
	s.db.QueryRow("SELECT COUNT(*) FROM measurements").Scan(&count)
	if count != 1 {
		t.Fatalf("retention should leave 1 row, got %d", count)
	}

	// The recent row should survive.
	var unix int64
	s.db.QueryRow("SELECT unix FROM measurements").Scan(&unix)
	if unix != recent {
		t.Fatalf("wrong row survived: got unix=%d, want %d", unix, recent)
	}
}

// (d) --db-path="" creates no file, nil Store safe everywhere.
func TestStoreNilSafe(t *testing.T) {
	var s *Store // nil

	// These must not panic.
	if s != nil {
		t.Fatal("nil store should be nil")
	}

	// The collector with nil store must work fine.
	c := collector.NewCollector()
	tt := proto.Telemetry{
		Kind: "ucast", Variation: "kernel", SrcIP: "a", DstIP: "b", Unix: 1,
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 10}},
	}
	e := c.Apply(tt)
	if e.Metrics.ServiceRTT.P50 != 10 {
		t.Fatalf("Apply failed with nil store")
	}

	// Record should be safe with nil store.
	RecordMeasurement(s, tt, 0)

	// SeedCollector with nil store does nothing.
	if err := seedFromStore(s, c, 60); err != nil {
		t.Fatalf("seedFromStore with nil store: %v", err)
	}
}

// (e) Drop path - full channel with stalled writer.
func TestStoreDropPath(t *testing.T) {
	dir := t.TempDir()
	s, err := OpenStore(filepath.Join(dir, "drop.db"), 7)
	if err != nil {
		t.Fatalf("OpenStore: %v", err)
	}
	defer s.Close()

	// Stop the writer so the channel fills up.
	s.stopWriter()

	// Fill the channel.
	row := measurementRow{
		Unix: time.Now().Unix(), Kind: "ucast", Variation: "kernel",
		SrcIP: "10.0.0.1", DstIP: "10.0.0.2", P50: 32,
		Messages: 100, LossPct: 0.0,
	}
	for i := 0; i < 4096; i++ {
		s.ch <- row
	}

	// Now RecordMeasurement should drop (non-blocking) and increment counter.
	before := atomic.LoadInt64(&s.Dropped)
	start := time.Now()
	for i := 0; i < 100; i++ {
		RecordMeasurement(s, proto.Telemetry{
			Kind: "ucast", Variation: "kernel", SrcIP: "a", DstIP: "b",
			Unix:    time.Now().Unix(),
			Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: int64(i)}},
		}, 0)
	}
	elapsed := time.Since(start)

	after := atomic.LoadInt64(&s.Dropped)
	if after-before < 100 {
		t.Fatalf("expected at least 100 drops, got %d", after-before)
	}
	// Must not block Apply - 100 iterations should complete in <50ms.
	if elapsed > 50*time.Millisecond {
		t.Fatalf("RecordMeasurement blocked: took %v", elapsed)
	}
}

// Helper: ensure OpenStore creates the tables.
func TestStoreSchemaCreation(t *testing.T) {
	s := openTestStore(t)

	// Verify tables exist.
	var name string
	err := s.db.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='runs'").Scan(&name)
	if err != nil {
		t.Fatalf("runs table not created: %v", err)
	}
	err = s.db.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='measurements'").Scan(&name)
	if err != nil {
		t.Fatalf("measurements table not created: %v", err)
	}

	// Verify indexes.
	rows, _ := s.db.Query("SELECT name FROM sqlite_master WHERE type='index' AND name LIKE 'idx_m_%'")
	defer rows.Close()
	idxNames := map[string]bool{}
	for rows.Next() {
		var n string
		rows.Scan(&n)
		idxNames[n] = true
	}
	for _, idx := range []string{"idx_m_edge", "idx_m_time", "idx_m_run"} {
		if !idxNames[idx] {
			t.Fatalf("index %s not found", idx)
		}
	}
}

// Verify batch flush on 200 rows.
func TestStoreBatchFlush(t *testing.T) {
	s := openTestStore(t)

	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 200, nil)
	for i := 0; i < 200; i++ {
		s.ch <- measurementRow{
			RunID: runID, Unix: time.Now().Unix(), Kind: "ucast", Variation: "kernel",
			SrcIP: "10.0.0.1", DstIP: "10.0.0.2", P50: int64(i),
			Messages: 100, LossPct: 0.0,
		}
	}
	// After 200 rows the batch should flush within 100ms.
	s.Flush()

	var count int
	s.db.QueryRow("SELECT COUNT(*) FROM measurements").Scan(&count)
	if count != 200 {
		t.Fatalf("expected 200 rows after batch flush, got %d", count)
	}
}

// Verify InsertRun writes params JSON correctly.
func TestStoreRunParams(t *testing.T) {
	s := openTestStore(t)

	params := map[string]any{"count": 10000, "rate": 10000}
	runID, err := s.InsertRun("ucast", "kernel", "among", `["i-1","i-2"]`, 2, params)
	if err != nil {
		t.Fatalf("InsertRun: %v", err)
	}

	var scope, targetIDs sql.NullString
	var pairsTotal int
	s.db.QueryRow("SELECT scope, target_ids, pairs_total FROM runs WHERE id=?", runID).
		Scan(&scope, &targetIDs, &pairsTotal)
	if scope.String != "among" {
		t.Fatalf("scope: want 'among', got %q", scope.String)
	}
	if targetIDs.String != `["i-1","i-2"]` {
		t.Fatalf("target_ids: got %q", targetIDs.String)
	}
	if pairsTotal != 2 {
		t.Fatalf("pairs_total: want 2, got %d", pairsTotal)
	}
}

// LatestMcastReplicatorResults must decode replicator identity out of the
// owning run's params JSON and return one row per measurement, across every
// replicator swept — this is what the report's "Per-replicator paths" table
// reads. Two replicators x one mode each, one destination.
func TestStoreLatestMcastReplicatorResults(t *testing.T) {
	s := openTestStore(t)

	runA, err := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl-a", "replicator_ip": "10.0.1.10",
		"replicator_pg": "cpg-a", "replicator_az": "eu-central-1a", "replicator_vpc": "vpc-a",
	})
	if err != nil {
		t.Fatalf("InsertRun A: %v", err)
	}
	runB, err := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl-b", "replicator_ip": "10.0.2.10",
		"replicator_pg": "cpg-b", "replicator_az": "eu-central-1b", "replicator_vpc": "vpc-b",
	})
	if err != nil {
		t.Fatalf("InsertRun B: %v", err)
	}

	now := time.Now().Unix()
	insertMeas := func(runID int64, p50 int64) {
		_, err := s.db.Exec(`
			INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, p90, p99, p999, max, loss_pct)
			VALUES (?, ?, 'mcast', 'copy', '10.0.0.1', '10.0.3.1', ?, ?, ?, ?, ?, 0.0)`,
			runID, now, p50, p50+2, p50+5, p50+8, p50+20)
		if err != nil {
			t.Fatalf("insert measurement: %v", err)
		}
	}
	insertMeas(runA, 40)
	insertMeas(runB, 65)

	results, err := s.LatestMcastReplicatorResults(0, 0)
	if err != nil {
		t.Fatalf("LatestMcastReplicatorResults: %v", err)
	}
	if len(results) != 2 {
		t.Fatalf("expected 2 results, got %d: %+v", len(results), results)
	}

	byRepl := map[string]McastReplicatorResult{}
	for _, r := range results {
		byRepl[r.ReplicatorIP] = r
	}
	a, ok := byRepl["10.0.1.10"]
	if !ok {
		t.Fatalf("missing replicator A result: %+v", results)
	}
	if a.ReplicatorID != "i-repl-a" || a.ReplicatorPG != "cpg-a" || a.ReplicatorAZ != "eu-central-1a" || a.ReplicatorVPC != "vpc-a" {
		t.Fatalf("replicator A identity not decoded correctly: %+v", a)
	}
	if a.P50 != 40 || a.Mode != "copy" {
		t.Fatalf("replicator A measurement wrong: %+v", a)
	}

	b, ok := byRepl["10.0.2.10"]
	if !ok {
		t.Fatalf("missing replicator B result: %+v", results)
	}
	if b.ReplicatorID != "i-repl-b" || b.ReplicatorPG != "cpg-b" || b.ReplicatorAZ != "eu-central-1b" {
		t.Fatalf("replicator B identity not decoded correctly: %+v", b)
	}
	if b.P50 != 65 {
		t.Fatalf("replicator B measurement wrong: %+v", b)
	}

	// sinceRunID floors which runs are considered: only runB should return.
	scoped, err := s.LatestMcastReplicatorResults(runB, 0)
	if err != nil {
		t.Fatalf("LatestMcastReplicatorResults scoped: %v", err)
	}
	if len(scoped) != 1 || scoped[0].ReplicatorIP != "10.0.2.10" {
		t.Fatalf("sinceRunID scoping failed: %+v", scoped)
	}
}

// A ucast run must never surface in the mcast-only replicator query.
func TestStoreLatestMcastReplicatorResultsExcludesUcast(t *testing.T) {
	s := openTestStore(t)

	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 1, map[string]any{"count": 100})
	_, err := s.db.Exec(`
		INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, loss_pct)
		VALUES (?, ?, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', 32, 0.0)`,
		runID, time.Now().Unix())
	if err != nil {
		t.Fatalf("insert: %v", err)
	}

	results, err := s.LatestMcastReplicatorResults(0, 0)
	if err != nil {
		t.Fatalf("LatestMcastReplicatorResults: %v", err)
	}
	if len(results) != 0 {
		t.Fatalf("expected 0 mcast results from a ucast run, got %d: %+v", len(results), results)
	}
}

// nil Store must be safe (persistence disabled), matching the rest of the API.
func TestStoreLatestMcastReplicatorResultsNilSafe(t *testing.T) {
	var s *Store
	results, err := s.LatestMcastReplicatorResults(0, 0)
	if err != nil {
		t.Fatalf("nil store should not error, got: %v", err)
	}
	if results != nil {
		t.Fatalf("nil store should return nil results, got: %+v", results)
	}
}

// LatestMeasurements must dedup to the newest row per (variation,src,dst,replicator)
// key, keeping two replicators measuring the same destination SEPARATE (unlike
// the live in-memory matrix, which can only hold one value per (src,dst)).
func TestStoreLatestMeasurementsDedupsPerReplicator(t *testing.T) {
	s := openTestStore(t)

	runA, _ := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl-a", "replicator_ip": "10.0.1.10",
		"replicator_pg": "cpg-a", "replicator_az": "eu-central-1a", "replicator_vpc": "vpc-a",
	})
	runB, _ := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl-b", "replicator_ip": "10.0.2.10",
		"replicator_pg": "cpg-b", "replicator_az": "eu-central-1b", "replicator_vpc": "vpc-b",
	})

	now := time.Now().Unix()
	insert := func(runID, unix, p50 int64) {
		_, err := s.db.Exec(`
			INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, loss_pct)
			VALUES (?, ?, 'mcast', 'copy', '10.0.0.1', '10.0.3.1', ?, 0.0)`, runID, unix, p50)
		if err != nil {
			t.Fatalf("insert: %v", err)
		}
	}
	// Two measurements through replicator A (older then newer) - only the
	// newer should survive dedup - plus one through replicator B.
	insert(runA, now-10, 999) // stale; must be dropped by dedup
	insert(runA, now, 40)
	insert(runB, now, 65)

	results, err := s.LatestMeasurements("mcast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(results) != 2 {
		t.Fatalf("expected 2 deduped results (one per replicator), got %d: %+v", len(results), results)
	}
	byRepl := map[string]MeasurementRow{}
	for _, r := range results {
		byRepl[r.ReplicatorIP] = r
	}
	a, ok := byRepl["10.0.1.10"]
	if !ok || a.P50 != 40 {
		t.Fatalf("replicator A should have the NEWER value (40), got: %+v", byRepl)
	}
	if a.ReplicatorVPC != "vpc-a" {
		t.Fatalf("replicator A VPC not decoded: %+v", a)
	}
	if a.ReplicatorPG != "cpg-a" || a.ReplicatorAZ != "eu-central-1a" {
		t.Fatalf("replicator A identity not decoded: %+v", a)
	}
	b, ok := byRepl["10.0.2.10"]
	if !ok || b.P50 != 65 {
		t.Fatalf("replicator B missing or wrong value: %+v", byRepl)
	}
}

// A ucast row has no replicator_ip in params, so it must key as "" and behave
// as plain "latest per pair" - not collide with, or be confused with, mcast.
func TestStoreLatestMeasurementsUcastHasNoReplicator(t *testing.T) {
	s := openTestStore(t)

	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 1, map[string]any{"count": 5000})
	now := time.Now().Unix()
	_, err := s.db.Exec(`
		INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, loss_pct)
		VALUES (?, ?, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', 32, 0.0)`, runID, now)
	if err != nil {
		t.Fatalf("insert: %v", err)
	}

	results, err := s.LatestMeasurements("ucast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result, got %d: %+v", len(results), results)
	}
	if results[0].ReplicatorIP != "" {
		t.Fatalf("ucast row should have empty ReplicatorIP, got %q", results[0].ReplicatorIP)
	}
	if results[0].P50 != 32 {
		t.Fatalf("unexpected p50: %+v", results[0])
	}
}

// sinceUnix must exclude measurements older than the floor.
func TestStoreLatestMeasurementsSinceUnixFloor(t *testing.T) {
	s := openTestStore(t)

	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 2, nil)
	now := time.Now().Unix()
	old := now - 100000
	_, err := s.db.Exec(`
		INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, loss_pct)
		VALUES (?, ?, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', 32, 0.0),
		       (?, ?, 'ucast', 'kernel', '10.0.0.3', '10.0.0.4', 40, 0.0)`,
		runID, old, runID, now)
	if err != nil {
		t.Fatalf("insert: %v", err)
	}

	results, err := s.LatestMeasurements("", now-10, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result after sinceUnix floor, got %d: %+v", len(results), results)
	}
	if results[0].SrcIP != "10.0.0.3" {
		t.Fatalf("wrong row survived the floor: %+v", results[0])
	}
}

// Topology enrichment: node metadata must be joined in from the nodes table
// for both endpoints when available.
func TestStoreLatestMeasurementsTopologyEnrichment(t *testing.T) {
	s := openTestStore(t)

	if err := s.UpsertNode(proto.NodeInfo{
		InstanceID: "i-src", PrivateIP: "10.0.0.1", Role: "source",
		AZ: "eu-central-1a", Region: "eu-central-1", VpcID: "vpc-1", PlacementGroup: "cpg-1",
	}, "1.0", "", 0); err != nil {
		t.Fatalf("UpsertNode src: %v", err)
	}
	if err := s.UpsertNode(proto.NodeInfo{
		InstanceID: "i-dst", PrivateIP: "10.0.0.2", Role: "destination",
		AZ: "eu-central-1b", Region: "eu-central-1", VpcID: "vpc-1", PlacementGroup: "cpg-2",
	}, "1.0", "", 0); err != nil {
		t.Fatalf("UpsertNode dst: %v", err)
	}

	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 1, nil)
	_, err := s.db.Exec(`
		INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, loss_pct)
		VALUES (?, ?, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', 32, 0.0)`, runID, time.Now().Unix())
	if err != nil {
		t.Fatalf("insert: %v", err)
	}

	results, err := s.LatestMeasurements("ucast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result, got %d", len(results))
	}
	r := results[0]
	if r.SrcRole != "source" || r.DstRole != "destination" {
		t.Fatalf("role enrichment wrong: src=%q dst=%q", r.SrcRole, r.DstRole)
	}
	if r.SrcAZ != "eu-central-1a" || r.DstAZ != "eu-central-1b" {
		t.Fatalf("az enrichment wrong: src=%q dst=%q", r.SrcAZ, r.DstAZ)
	}
	if r.SrcPG != "cpg-1" || r.DstPG != "cpg-2" {
		t.Fatalf("pg enrichment wrong: src=%q dst=%q", r.SrcPG, r.DstPG)
	}
}

// A row whose endpoints are not in the nodes table must still be returned
// (measurement stays valid), just without topology fields populated.
func TestStoreLatestMeasurementsUnknownNodeIsBestEffort(t *testing.T) {
	s := openTestStore(t)

	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 1, nil)
	_, err := s.db.Exec(`
		INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, loss_pct)
		VALUES (?, ?, 'ucast', 'kernel', '10.9.9.1', '10.9.9.2', 50, 0.0)`, runID, time.Now().Unix())
	if err != nil {
		t.Fatalf("insert: %v", err)
	}

	results, err := s.LatestMeasurements("ucast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result even with unknown nodes, got %d", len(results))
	}
	if results[0].SrcRole != "" || results[0].DstRole != "" {
		t.Fatalf("expected blank topology for unknown nodes, got: %+v", results[0])
	}
}

// nil Store must be safe.
func TestStoreLatestMeasurementsNilSafe(t *testing.T) {
	var s *Store
	results, err := s.LatestMeasurements("ucast", 0, 0)
	if err != nil {
		t.Fatalf("nil store should not error, got: %v", err)
	}
	if results != nil {
		t.Fatalf("nil store should return nil results, got: %+v", results)
	}
}

// A measurements table created before hop1/hop2 existed must gain those
// columns via migrateAddHopColumns on the next OpenStore, without losing
// existing rows or erroring on a table that already has them (idempotent).
func TestStoreMigrateAddHopColumnsIsIdempotent(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "migrate.db")

	// Simulate a pre-hop-columns database: open once (creates the full
	// current schema, hop columns included), then drop them back out by
	// rebuilding a stripped-down table, to exercise the migration path.
	s1, err := OpenStore(path, 7)
	if err != nil {
		t.Fatalf("OpenStore (create): %v", err)
	}
	if _, err := s1.db.Exec(`
		DROP TABLE measurements;
		CREATE TABLE measurements (
			id INTEGER PRIMARY KEY AUTOINCREMENT, run_id INTEGER, unix INTEGER NOT NULL,
			kind TEXT NOT NULL, variation TEXT NOT NULL, src_ip TEXT NOT NULL, dst_ip TEXT NOT NULL,
			tx_mode TEXT, p50 INTEGER, p90 INTEGER, p99 INTEGER, p999 INTEGER, max INTEGER,
			min INTEGER, mean INTEGER, messages INTEGER, lost INTEGER, loss_pct REAL, cmd_id TEXT
		);`); err != nil {
		t.Fatalf("simulate pre-migration schema: %v", err)
	}
	// A row inserted before the migration must survive it.
	if _, err := s1.db.Exec(`INSERT INTO measurements (unix, kind, variation, src_ip, dst_ip, p50)
		VALUES (?, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', 32)`, time.Now().Unix()); err != nil {
		t.Fatalf("insert pre-migration row: %v", err)
	}
	s1.Close()

	// Re-opening must run the migration and add the columns without erroring
	// or losing the pre-existing row.
	s2, err := OpenStore(path, 7)
	if err != nil {
		t.Fatalf("OpenStore (migrate): %v", err)
	}
	defer s2.Close()

	rows, err := s2.db.Query(`PRAGMA table_info(measurements)`)
	if err != nil {
		t.Fatalf("table_info: %v", err)
	}
	cols := map[string]bool{}
	for rows.Next() {
		var cid int
		var name, ctype string
		var notnull, pk int
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
			t.Fatalf("scan table_info: %v", err)
		}
		cols[name] = true
	}
	rows.Close()
	for _, col := range []string{"hop1_p50", "hop1_p99", "hop1_p999", "hop2_p50", "hop2_p99", "hop2_p999"} {
		if !cols[col] {
			t.Fatalf("migration did not add column %s: %+v", col, cols)
		}
	}

	var count int
	if err := s2.db.QueryRow(`SELECT COUNT(*) FROM measurements`).Scan(&count); err != nil {
		t.Fatalf("count: %v", err)
	}
	if count != 1 {
		t.Fatalf("expected the pre-migration row to survive, got %d rows", count)
	}

	// A third open (columns already present) must not error - the idempotent case.
	if err := migrateAddHopColumns(s2.db); err != nil {
		t.Fatalf("re-running migration on an already-migrated table should be a no-op, got: %v", err)
	}
}

// A mcast measurement with a hop split must persist hop1/hop2 and round-trip
// through the write channel + writer, not just direct SQL inserts. Also
// verifies LatestMeasurements and LatestMcastReplicatorResults both surface
// the hop fields on read, not just the raw table.
func TestStoreRecordMeasurementPersistsHopSplit(t *testing.T) {
	s := openTestStore(t)

	runID, _ := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl", "replicator_ip": "10.0.9.9",
	})
	RecordMeasurement(s, proto.Telemetry{
		Kind: "mcast", Variation: "copy", SrcIP: "10.0.0.1", DstIP: "10.0.0.2", Unix: time.Now().Unix(),
		Metrics: proto.Metrics{
			ServiceRTT: proto.Pct{P50: 60},
			Hop1:       &proto.HopPct{P50: 26, P99: 42, P999: 55},
			Hop2:       &proto.HopPct{P50: 34, P99: 49, P999: 63},
		},
	}, runID)
	s.Flush()

	var hop1p50, hop1p99, hop1p999, hop2p50, hop2p99, hop2p999 sql.NullInt64
	err := s.db.QueryRow(`SELECT hop1_p50, hop1_p99, hop1_p999, hop2_p50, hop2_p99, hop2_p999 FROM measurements
		WHERE src_ip='10.0.0.1' AND dst_ip='10.0.0.2'`).
		Scan(&hop1p50, &hop1p99, &hop1p999, &hop2p50, &hop2p99, &hop2p999)
	if err != nil {
		t.Fatalf("query: %v", err)
	}
	if !hop1p50.Valid || hop1p50.Int64 != 26 || !hop1p99.Valid || hop1p99.Int64 != 42 || !hop1p999.Valid || hop1p999.Int64 != 55 {
		t.Fatalf("hop1 not persisted correctly: p50=%+v p99=%+v p999=%+v", hop1p50, hop1p99, hop1p999)
	}
	if !hop2p50.Valid || hop2p50.Int64 != 34 || !hop2p99.Valid || hop2p99.Int64 != 49 || !hop2p999.Valid || hop2p999.Int64 != 63 {
		t.Fatalf("hop2 not persisted correctly: p50=%+v p99=%+v p999=%+v", hop2p50, hop2p99, hop2p999)
	}

	// LatestMeasurements must surface the same hop values on read.
	measResults, err := s.LatestMeasurements("mcast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(measResults) != 1 {
		t.Fatalf("expected 1 result, got %d", len(measResults))
	}
	mr := measResults[0]
	if mr.Hop1P50 == nil || *mr.Hop1P50 != 26 || mr.Hop2P50 == nil || *mr.Hop2P50 != 34 {
		t.Fatalf("LatestMeasurements did not surface hop values: %+v", mr)
	}
	if mr.Hop1P999 == nil || *mr.Hop1P999 != 55 || mr.Hop2P999 == nil || *mr.Hop2P999 != 63 {
		t.Fatalf("LatestMeasurements did not surface hop p999 values: %+v", mr)
	}

	// LatestMcastReplicatorResults must surface the same hop values too.
	replResults, err := s.LatestMcastReplicatorResults(0, 0)
	if err != nil {
		t.Fatalf("LatestMcastReplicatorResults: %v", err)
	}
	if len(replResults) != 1 {
		t.Fatalf("expected 1 replicator result, got %d", len(replResults))
	}
	rr := replResults[0]
	if rr.Hop1P50 == nil || *rr.Hop1P50 != 26 || rr.Hop2P99 == nil || *rr.Hop2P99 != 49 {
		t.Fatalf("LatestMcastReplicatorResults did not surface hop values: %+v", rr)
	}
	if rr.Hop1P999 == nil || *rr.Hop1P999 != 55 || rr.Hop2P999 == nil || *rr.Hop2P999 != 63 {
		t.Fatalf("LatestMcastReplicatorResults did not surface hop p999 values: %+v", rr)
	}
}

// A mcast run's params.size (mcast_send payload bytes, set by
// RunMcastMatrix) must round-trip through both LatestMeasurements and
// LatestMcastReplicatorResults, the same way replicator_id/pg/az/vpc already
// do - size is stored in the same params JSON blob via json_extract, not a
// dedicated column.
func TestStoreRecordMeasurementPersistsSize(t *testing.T) {
	t.Parallel()
	s := openTestStore(t)

	runID, _ := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl", "replicator_ip": "10.0.9.9", "size": 512,
	})
	RecordMeasurement(s, proto.Telemetry{
		Kind: "mcast", Variation: "copy", SrcIP: "10.0.0.1", DstIP: "10.0.0.2", Unix: time.Now().Unix(),
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 41}},
	}, runID)
	s.Flush()

	measResults, err := s.LatestMeasurements("mcast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(measResults) != 1 {
		t.Fatalf("expected 1 result, got %d", len(measResults))
	}
	if measResults[0].Size != 512 {
		t.Fatalf("LatestMeasurements did not surface size: got %d, want 512", measResults[0].Size)
	}

	replResults, err := s.LatestMcastReplicatorResults(0, 0)
	if err != nil {
		t.Fatalf("LatestMcastReplicatorResults: %v", err)
	}
	if len(replResults) != 1 {
		t.Fatalf("expected 1 replicator result, got %d", len(replResults))
	}
	if replResults[0].Size != 512 {
		t.Fatalf("LatestMcastReplicatorResults did not surface size: got %d, want 512", replResults[0].Size)
	}
}

// A run whose params carry no size (ucast, or a mcast run predating this
// field) must read back as 0, not error - 0 also means "tool default" on the
// write side, so the two cases are indistinguishable by design (see
// McastMatrixParams.Size).
func TestStoreRecordMeasurementMissingSizeReadsAsZero(t *testing.T) {
	t.Parallel()
	s := openTestStore(t)

	runID, _ := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl", "replicator_ip": "10.0.9.9",
	})
	RecordMeasurement(s, proto.Telemetry{
		Kind: "mcast", Variation: "copy", SrcIP: "10.0.0.1", DstIP: "10.0.0.2", Unix: time.Now().Unix(),
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 41}},
	}, runID)
	s.Flush()

	measResults, err := s.LatestMeasurements("mcast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(measResults) != 1 || measResults[0].Size != 0 {
		t.Fatalf("expected size=0 for a run with no size param, got %+v", measResults)
	}
}

// AchievedPps/RequestedPps/RateShortfall must round-trip through
// LatestMeasurements when the Telemetry carries them (dev/roadmap/fix.md's "Report
// achieved vs requested rate" item), and RateShortfall specifically must
// distinguish "computed false" from "not computed" - both look like a zero
// value on the wire, but only the NULL/nil case should read back as nil.
func TestStoreRecordMeasurementPersistsRateFields(t *testing.T) {
	t.Parallel()
	s := openTestStore(t)

	runID, _ := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl", "replicator_ip": "10.0.9.9",
	})
	RecordMeasurement(s, proto.Telemetry{
		Kind: "mcast", Variation: "copy", SrcIP: "10.0.0.1", DstIP: "10.0.0.2", Unix: time.Now().Unix(),
		Metrics: proto.Metrics{
			ServiceRTT: proto.Pct{P50: 41},
			// A shortfall case: 50k achieved of 100k requested.
			AchievedPps: 50000, RequestedPps: 100000, RateShortfall: true,
		},
	}, runID)
	s.Flush()

	measResults, err := s.LatestMeasurements("mcast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(measResults) != 1 {
		t.Fatalf("expected 1 result, got %d", len(measResults))
	}
	r := measResults[0]
	if r.AchievedPps == nil || *r.AchievedPps != 50000 {
		t.Fatalf("AchievedPps not persisted correctly: %+v", r.AchievedPps)
	}
	if r.RequestedPps == nil || *r.RequestedPps != 100000 {
		t.Fatalf("RequestedPps not persisted correctly: %+v", r.RequestedPps)
	}
	if r.RateShortfall == nil || !*r.RateShortfall {
		t.Fatalf("RateShortfall not persisted correctly (want true): %+v", r.RateShortfall)
	}
}

// A run with no rate-expectation data at all (unbounded-rate run, or a
// measurement predating this field) must read back as nil for all three
// fields, not a misleading computed zero.
func TestStoreRecordMeasurementMissingRateFieldsReadAsNil(t *testing.T) {
	t.Parallel()
	s := openTestStore(t)

	runID, _ := s.InsertRun("mcast", "copy", "", "", 1, map[string]any{
		"replicator_id": "i-repl", "replicator_ip": "10.0.9.9",
	})
	RecordMeasurement(s, proto.Telemetry{
		Kind: "mcast", Variation: "copy", SrcIP: "10.0.0.1", DstIP: "10.0.0.2", Unix: time.Now().Unix(),
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 41}},
	}, runID)
	s.Flush()

	measResults, err := s.LatestMeasurements("mcast", 0, 0)
	if err != nil {
		t.Fatalf("LatestMeasurements: %v", err)
	}
	if len(measResults) != 1 {
		t.Fatalf("expected 1 result, got %d", len(measResults))
	}
	r := measResults[0]
	if r.AchievedPps != nil || r.RequestedPps != nil || r.RateShortfall != nil {
		t.Fatalf("expected all rate fields nil when not computed, got %+v", r)
	}
}

// A ucast measurement (no Hop1/Hop2 on the Telemetry) must persist NULL hop
// columns, not zeroes - zero is a valid latency value and would be
// indistinguishable from "measured 0us", which ucast never has.
func TestStoreRecordMeasurementUcastHasNullHops(t *testing.T) {
	s := openTestStore(t)

	runID, _ := s.InsertRun("ucast", "kernel", "full", "", 1, nil)
	RecordMeasurement(s, proto.Telemetry{
		Kind: "ucast", Variation: "kernel", SrcIP: "10.0.0.1", DstIP: "10.0.0.2", Unix: time.Now().Unix(),
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 32}},
	}, runID)
	s.Flush()

	var hop1p50 sql.NullInt64
	err := s.db.QueryRow(`SELECT hop1_p50 FROM measurements WHERE src_ip='10.0.0.1' AND dst_ip='10.0.0.2'`).
		Scan(&hop1p50)
	if err != nil {
		t.Fatalf("query: %v", err)
	}
	if hop1p50.Valid {
		t.Fatalf("ucast measurement should have NULL hop1_p50, got %d", hop1p50.Int64)
	}
}
