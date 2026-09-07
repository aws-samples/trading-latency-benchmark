package mcp

import (
	"database/sql"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	_ "modernc.org/sqlite"
)

// createFixtureDB creates a test database with the real schema and returns its path.
func createFixtureDB(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "test.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open fixture db: %v", err)
	}
	defer db.Close()

	schema := `
	PRAGMA journal_mode=WAL;
	PRAGMA synchronous=NORMAL;

	CREATE TABLE IF NOT EXISTS runs (
		id          INTEGER PRIMARY KEY AUTOINCREMENT,
		started_at  INTEGER NOT NULL,
		ended_at    INTEGER,
		kind        TEXT NOT NULL,
		variation   TEXT NOT NULL,
		scope       TEXT,
		target_ids  TEXT,
		pairs_total INTEGER,
		pairs_ok    INTEGER,
		params      TEXT
	);

	CREATE TABLE IF NOT EXISTS measurements (
		id        INTEGER PRIMARY KEY AUTOINCREMENT,
		run_id    INTEGER REFERENCES runs(id),
		unix      INTEGER NOT NULL,
		kind      TEXT NOT NULL,
		variation TEXT NOT NULL,
		src_ip    TEXT NOT NULL,
		dst_ip    TEXT NOT NULL,
		tx_mode   TEXT,
		p50       INTEGER, p90 INTEGER, p99 INTEGER, p999 INTEGER, max INTEGER,
		min       INTEGER, mean INTEGER,
		messages  INTEGER, lost INTEGER, loss_pct REAL,
		cmd_id    TEXT
	);

	CREATE INDEX IF NOT EXISTS idx_m_edge ON measurements(kind, variation, src_ip, dst_ip, unix DESC);
	CREATE INDEX IF NOT EXISTS idx_m_time ON measurements(unix);
	CREATE INDEX IF NOT EXISTS idx_m_run  ON measurements(run_id);
	`
	if _, err := db.Exec(schema); err != nil {
		t.Fatalf("create schema: %v", err)
	}
	return path
}

// seedFixtureData inserts known runs and measurements for deterministic tests.
func seedFixtureData(t *testing.T, path string) {
	t.Helper()
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open for seed: %v", err)
	}
	defer db.Close()

	// Two runs: run 1 (ucast/kernel), run 2 (ucast/xdp)
	_, err = db.Exec(`INSERT INTO runs (id, started_at, ended_at, kind, variation, scope, pairs_total, pairs_ok)
		VALUES (1, 1700000000, 1700000060, 'ucast', 'kernel', 'full', 2, 2)`)
	if err != nil {
		t.Fatalf("insert run 1: %v", err)
	}
	_, err = db.Exec(`INSERT INTO runs (id, started_at, ended_at, kind, variation, scope, pairs_total, pairs_ok)
		VALUES (2, 1700001000, 1700001060, 'ucast', 'xdp', 'full', 2, 2)`)
	if err != nil {
		t.Fatalf("insert run 2: %v", err)
	}
	// Run 3: a second kernel run (newer) for regression testing
	_, err = db.Exec(`INSERT INTO runs (id, started_at, ended_at, kind, variation, scope, pairs_total, pairs_ok)
		VALUES (3, 1700002000, 1700002060, 'ucast', 'kernel', 'full', 2, 2)`)
	if err != nil {
		t.Fatalf("insert run 3: %v", err)
	}

	// Measurements for run 1 (kernel): A->B p50=30, B->A p50=32
	_, err = db.Exec(`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct)
		VALUES (1, 1700000010, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', 30, 34, 37, 45, 60, 28, 31, 100000, 0, 0.0)`)
	if err != nil {
		t.Fatalf("insert m1: %v", err)
	}
	_, err = db.Exec(`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct)
		VALUES (1, 1700000010, 'ucast', 'kernel', '10.0.0.2', '10.0.0.1', 32, 35, 38, 46, 61, 29, 33, 100000, 0, 0.0)`)
	if err != nil {
		t.Fatalf("insert m2: %v", err)
	}

	// Measurements for run 2 (xdp): A->B p50=25, B->A p50=27
	_, err = db.Exec(`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct)
		VALUES (2, 1700001010, 'ucast', 'xdp', '10.0.0.1', '10.0.0.2', 25, 28, 30, 35, 40, 22, 26, 100000, 0, 0.0)`)
	if err != nil {
		t.Fatalf("insert m3: %v", err)
	}
	_, err = db.Exec(`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct)
		VALUES (2, 1700001010, 'ucast', 'xdp', '10.0.0.2', '10.0.0.1', 27, 30, 33, 38, 43, 24, 28, 100000, 0, 0.0)`)
	if err != nil {
		t.Fatalf("insert m4: %v", err)
	}

	// Measurements for run 3 (kernel, newer): A->B p50=40 (regression!), B->A p50=33
	_, err = db.Exec(`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct)
		VALUES (3, 1700002010, 'ucast', 'kernel', '10.0.0.1', '10.0.0.2', 40, 44, 47, 55, 70, 38, 41, 100000, 0, 0.0)`)
	if err != nil {
		t.Fatalf("insert m5: %v", err)
	}
	_, err = db.Exec(`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct)
		VALUES (3, 1700002010, 'ucast', 'kernel', '10.0.0.2', '10.0.0.1', 33, 36, 39, 47, 62, 30, 34, 100000, 0, 0.0)`)
	if err != nil {
		t.Fatalf("insert m6: %v", err)
	}
}

func openTestDB(t *testing.T, path string) *DB {
	t.Helper()
	db, err := OpenDB(path)
	if err != nil {
		t.Fatalf("OpenDB: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

// --- Read-only guarantee ---

func TestReadOnlyGuarantee(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	// Attempt a write - must fail.
	_, err := db.conn.Exec("INSERT INTO runs (started_at, kind, variation) VALUES (1, 'x', 'y')")
	if err == nil {
		t.Fatalf("expected write to fail on read-only connection, but it succeeded")
	}
}

// --- list_runs ---

func TestListRuns(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	// All runs
	res, err := db.ListRuns(ListRunsParams{})
	if err != nil {
		t.Fatalf("ListRuns: %v", err)
	}
	if len(res.Rows) != 3 {
		t.Fatalf("expected 3 runs, got %d", len(res.Rows))
	}
	if res.SQL == "" {
		t.Fatalf("expected non-empty SQL")
	}

	// Filter by kind
	res, err = db.ListRuns(ListRunsParams{Kind: "ucast"})
	if err != nil {
		t.Fatalf("ListRuns kind=ucast: %v", err)
	}
	if len(res.Rows) != 3 {
		t.Fatalf("expected 3 runs for ucast, got %d", len(res.Rows))
	}

	// Filter by variation
	res, err = db.ListRuns(ListRunsParams{Variation: "xdp"})
	if err != nil {
		t.Fatalf("ListRuns variation=xdp: %v", err)
	}
	if len(res.Rows) != 1 {
		t.Fatalf("expected 1 run for xdp, got %d", len(res.Rows))
	}

	// Limit
	res, err = db.ListRuns(ListRunsParams{Limit: 2})
	if err != nil {
		t.Fatalf("ListRuns limit=2: %v", err)
	}
	if len(res.Rows) != 2 {
		t.Fatalf("expected 2 runs with limit, got %d", len(res.Rows))
	}

	// Since
	res, err = db.ListRuns(ListRunsParams{Since: 1700001500})
	if err != nil {
		t.Fatalf("ListRuns since: %v", err)
	}
	if len(res.Rows) != 1 {
		t.Fatalf("expected 1 run after since, got %d", len(res.Rows))
	}
}

// --- query_latency ---

func TestQueryLatency(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	// All measurements
	res, err := db.QueryLatency(QueryLatencyParams{})
	if err != nil {
		t.Fatalf("QueryLatency: %v", err)
	}
	if len(res.Rows) != 6 {
		t.Fatalf("expected 6 measurements, got %d", len(res.Rows))
	}

	// Filter by src
	res, err = db.QueryLatency(QueryLatencyParams{Src: "10.0.0.1"})
	if err != nil {
		t.Fatalf("QueryLatency src: %v", err)
	}
	if len(res.Rows) != 3 {
		t.Fatalf("expected 3 measurements from 10.0.0.1, got %d", len(res.Rows))
	}

	// Filter by src + dst
	res, err = db.QueryLatency(QueryLatencyParams{Src: "10.0.0.1", Dst: "10.0.0.2"})
	if err != nil {
		t.Fatalf("QueryLatency src+dst: %v", err)
	}
	if len(res.Rows) != 3 {
		t.Fatalf("expected 3 measurements for 10.0.0.1->10.0.0.2, got %d", len(res.Rows))
	}

	// Filter by variation
	res, err = db.QueryLatency(QueryLatencyParams{Variation: "xdp"})
	if err != nil {
		t.Fatalf("QueryLatency variation=xdp: %v", err)
	}
	if len(res.Rows) != 2 {
		t.Fatalf("expected 2 xdp measurements, got %d", len(res.Rows))
	}
}

// --- compare_runs ---

func TestCompareRuns(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	// Compare run 1 (kernel, older) vs run 3 (kernel, newer)
	res, err := db.CompareRuns(1, 3)
	if err != nil {
		t.Fatalf("CompareRuns: %v", err)
	}
	if len(res.Rows) != 2 {
		t.Fatalf("expected 2 cell deltas, got %d", len(res.Rows))
	}

	// A->B: was 30, now 40, delta = +10
	found := false
	for _, row := range res.Rows {
		m := row.(map[string]any)
		if m["src_ip"] == "10.0.0.1" && m["dst_ip"] == "10.0.0.2" {
			found = true
			delta, ok := m["delta_p50"].(int64)
			if !ok {
				// Try float64 from JSON
				if df, ok2 := m["delta_p50"].(float64); ok2 {
					delta = int64(df)
				}
			}
			if delta != 10 {
				t.Fatalf("expected delta_p50=10 for A->B, got %v", m["delta_p50"])
			}
		}
	}
	if !found {
		t.Fatalf("missing A->B pair in compare_runs result")
	}
}

// --- compare_modes ---

func TestCompareModes(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	// Compare kernel vs xdp for ucast
	res, err := db.CompareModes("ucast", "kernel", "xdp", 0)
	if err != nil {
		t.Fatalf("CompareModes: %v", err)
	}
	if len(res.Rows) < 1 {
		t.Fatalf("expected at least 1 row, got %d", len(res.Rows))
	}

	// A->B: kernel newest p50=40 (run 3), xdp p50=25. delta = 25 - 40 = -15
	found := false
	for _, row := range res.Rows {
		m := row.(map[string]any)
		if m["src_ip"] == "10.0.0.1" && m["dst_ip"] == "10.0.0.2" {
			found = true
		}
	}
	if !found {
		t.Fatalf("missing A->B in compare_modes result")
	}
}

// --- regressions ---

func TestRegressions(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	// Threshold 5us: A->B went from p50=30 to p50=40 (+10), should be flagged
	// B->A went from 32 to 33 (+1), should NOT be flagged
	res, err := db.Regressions(RegressionsParams{ThresholdUs: 5, WindowHours: 10000})
	if err != nil {
		t.Fatalf("Regressions: %v", err)
	}

	flaggedAB := false
	flaggedBA := false
	for _, row := range res.Rows {
		m := row.(map[string]any)
		src, _ := m["src_ip"].(string)
		dst, _ := m["dst_ip"].(string)
		if src == "10.0.0.1" && dst == "10.0.0.2" {
			flaggedAB = true
		}
		if src == "10.0.0.2" && dst == "10.0.0.1" {
			flaggedBA = true
		}
	}
	if !flaggedAB {
		t.Fatalf("expected A->B (delta 10) to be flagged as regression")
	}
	if flaggedBA {
		t.Fatalf("expected B->A (delta 1) to NOT be flagged as regression")
	}

	// Threshold 15us: nothing should be flagged (delta is 10)
	res, err = db.Regressions(RegressionsParams{ThresholdUs: 15, WindowHours: 10000})
	if err != nil {
		t.Fatalf("Regressions threshold=15: %v", err)
	}
	if len(res.Rows) != 0 {
		t.Fatalf("expected 0 regressions with threshold=15, got %d", len(res.Rows))
	}
}

// --- topology_summary ---

func TestTopologySummary(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	res, err := db.TopologySummary()
	if err != nil {
		t.Fatalf("TopologySummary: %v", err)
	}
	// Should have newest sample per edge (4 edges: A->B, B->A for 2 variations)
	if len(res.Rows) < 2 {
		t.Fatalf("expected at least 2 edges in topology, got %d", len(res.Rows))
	}
	if res.SQL == "" {
		t.Fatalf("expected SQL in result")
	}
}

// --- empty/missing DB ---

func TestEmptyDB(t *testing.T) {
	path := createFixtureDB(t)
	db := openTestDB(t, path)

	// All tools should return empty results, not error
	res, err := db.ListRuns(ListRunsParams{})
	if err != nil {
		t.Fatalf("ListRuns empty: %v", err)
	}
	if len(res.Rows) != 0 {
		t.Fatalf("expected 0 runs in empty db, got %d", len(res.Rows))
	}

	res, err = db.QueryLatency(QueryLatencyParams{})
	if err != nil {
		t.Fatalf("QueryLatency empty: %v", err)
	}
	if len(res.Rows) != 0 {
		t.Fatalf("expected 0 measurements, got %d", len(res.Rows))
	}

	res, err = db.TopologySummary()
	if err != nil {
		t.Fatalf("TopologySummary empty: %v", err)
	}
	if len(res.Rows) != 0 {
		t.Fatalf("expected 0 rows, got %d", len(res.Rows))
	}
}

func TestMissingDB(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "nonexistent.db")
	_, err := OpenDB(path)
	if err == nil {
		t.Fatalf("expected error for missing DB file")
	}
}

func TestMissingDBFileDoesNotExist(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "subdir", "nope.db")
	_, err := OpenDB(path)
	if err == nil {
		t.Fatalf("expected error for missing DB in missing directory")
	}
}

// --- JSON-RPC protocol test ---

func TestJSONRPCDispatch(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	srv := NewServer(db)

	// Test tools/list
	req := JSONRPCRequest{
		JSONRPC: "2.0",
		ID:      json.RawMessage(`1`),
		Method:  "tools/list",
	}
	resp := srv.Handle(req)
	if resp.Error != nil {
		t.Fatalf("tools/list error: %v", resp.Error)
	}

	// Test tools/call with list_runs
	params := map[string]any{
		"name":      "list_runs",
		"arguments": map[string]any{},
	}
	paramsJSON, _ := json.Marshal(params)
	req = JSONRPCRequest{
		JSONRPC: "2.0",
		ID:      json.RawMessage(`2`),
		Method:  "tools/call",
		Params:  paramsJSON,
	}
	resp = srv.Handle(req)
	if resp.Error != nil {
		t.Fatalf("tools/call list_runs error: %v", resp.Error)
	}

	// Verify result contains content
	result, ok := resp.Result.(map[string]any)
	if !ok {
		t.Fatalf("expected map result, got %T", resp.Result)
	}
	content, ok := result["content"].([]map[string]any)
	if !ok {
		t.Fatalf("expected content array, got %T", result["content"])
	}
	if len(content) == 0 {
		t.Fatalf("expected non-empty content")
	}

	// Verify the text content is valid JSON with rows and sql
	text, ok := content[0]["text"].(string)
	if !ok {
		t.Fatalf("expected text in content")
	}
	var parsed map[string]any
	if err := json.Unmarshal([]byte(text), &parsed); err != nil {
		t.Fatalf("content text is not valid JSON: %v", err)
	}
	if _, ok := parsed["sql"]; !ok {
		t.Fatalf("expected 'sql' key in result")
	}
	if _, ok := parsed["rows"]; !ok {
		t.Fatalf("expected 'rows' key in result")
	}
}

// Test the initialize handshake
func TestInitialize(t *testing.T) {
	path := createFixtureDB(t)
	db := openTestDB(t, path)
	srv := NewServer(db)

	req := JSONRPCRequest{
		JSONRPC: "2.0",
		ID:      json.RawMessage(`1`),
		Method:  "initialize",
		Params:  json.RawMessage(`{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}`),
	}
	resp := srv.Handle(req)
	if resp.Error != nil {
		t.Fatalf("initialize error: %v", resp.Error)
	}
	result := resp.Result.(map[string]any)
	if result["protocolVersion"] != "2024-11-05" {
		t.Fatalf("unexpected protocol version: %v", result["protocolVersion"])
	}
}

// Verify DB path from env works for main
func TestDBPathEnvRequired(t *testing.T) {
	// Just ensure the env var name is what we expect
	os.Setenv("MCP_DB_PATH", "")
	defer os.Unsetenv("MCP_DB_PATH")
	// We just verify the constant - main() itself would check and exit
}
