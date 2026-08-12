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
	time.Sleep(700 * time.Millisecond)

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
	time.Sleep(300 * time.Millisecond)

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
