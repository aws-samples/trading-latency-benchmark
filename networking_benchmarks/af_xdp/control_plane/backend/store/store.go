package store

import (
	"database/sql"
	"encoding/json"
	"log"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"afxdp-cp/backend/collector"
	"afxdp-cp/proto"

	_ "modernc.org/sqlite"
)

// measurementRow is the write-channel payload.
type measurementRow struct {
	RunID     int64
	Unix      int64
	Kind      string
	Variation string
	SrcIP     string
	DstIP     string
	TxMode    string
	P50       int64
	P90       int64
	P99       int64
	P999      int64
	Max       int64
	Min       int64
	Mean      int64
	Messages  int64
	Lost      int64
	LossPct   float64
	CmdID     string
	// Hop1/Hop2: mcast one-way path split (source->replicator,
	// replicator->destination). nil for ucast and for any mcast result whose
	// wire header carried no replicator timestamp.
	Hop1P50 *int64
	Hop1P99 *int64
	Hop1P999 *int64
	Hop2P50 *int64
	Hop2P99 *int64
	Hop2P999 *int64
}

// Store provides SQLite persistence for measurement history.
type Store struct {
	db      *sql.DB
	ch      chan measurementRow
	Dropped int64 // atomic; exported for testing

	stopCh chan struct{}
	wg     sync.WaitGroup

	// Throttle drop-log to once per minute.
	lastDropLog atomic.Int64

	// currentRun is the campaign ingest attributes measurements to. Telemetry
	// arrives on the NATS goroutine while the orchestrator drives the campaign,
	// so this is the handoff between them. 0 means "outside any campaign".
	currentRun atomic.Int64

	// flushReq carries a caller's ack channel to the writer, which commits at
	// once and closes it. This is what makes Flush deterministic.
	flushReq chan chan struct{}
}

// SetCurrentRun records which campaign subsequent telemetry belongs to.
// Safe on a nil Store so callers need no persistence check.
func (s *Store) SetCurrentRun(id int64) {
	if s == nil {
		return
	}
	s.currentRun.Store(id)
}

// CurrentRun returns the campaign telemetry is currently attributed to, or 0
// when none is running or persistence is disabled.
func (s *Store) CurrentRun() int64 {
	if s == nil {
		return 0
	}
	return s.currentRun.Load()
}

// Flush blocks until the writer has committed everything queued at call time.
//
// It asks the writer to commit immediately rather than waiting for the batch
// timer, so a caller never pays batchTimeout just to observe its own write.
// Shutdown and tests both need that guarantee; polling or sleeping instead
// makes tests slow and, worse, flaky.
func (s *Store) Flush() {
	if s == nil {
		return
	}
	ack := make(chan struct{})
	select {
	case s.flushReq <- ack:
	case <-s.stopCh:
		return // writer is gone; nothing can still be queued
	case <-time.After(5 * time.Second):
		return
	}
	select {
	case <-ack:
	case <-time.After(5 * time.Second):
	}
}

const (
	storeChanCap = 4096
	batchSize    = 200
	batchTimeout = 500 * time.Millisecond
)

// OpenStore opens (or creates) the SQLite database and starts the writer.
func OpenStore(path string, retentionDays int) (*Store, error) {
	db, err := sql.Open("sqlite", path+"?_journal_mode=WAL&_synchronous=NORMAL")
	if err != nil {
		return nil, err
	}
	// Single connection - SQLite serializes writes anyway.
	db.SetMaxOpenConns(2)

	if err := createSchema(db); err != nil {
		db.Close()
		return nil, err
	}

	s := &Store{
		db:       db,
		ch:       make(chan measurementRow, storeChanCap),
		flushReq: make(chan chan struct{}, 8),
		stopCh:   make(chan struct{}),
	}
	s.wg.Add(1)
	go s.writer()

	if retentionDays > 0 {
		s.wg.Add(1)
		go s.retentionLoop(retentionDays)
	}
	return s, nil
}

func createSchema(db *sql.DB) error {
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
		cmd_id    TEXT,
		hop1_p50  INTEGER, hop1_p99 INTEGER, hop1_p999 INTEGER,
		hop2_p50  INTEGER, hop2_p99 INTEGER, hop2_p999 INTEGER
	);

	CREATE TABLE IF NOT EXISTS nodes (
		instance_id      TEXT PRIMARY KEY,
		private_ip       TEXT NOT NULL,
		public_ip        TEXT,
		hostname         TEXT,
		role             TEXT,
		stack            TEXT,
		region           TEXT,
		az               TEXT,
		vpc_id           TEXT,
		subnet_id        TEXT,
		placement_group  TEXT,
		pg_strategy      TEXT,
		instance_type    TEXT,
		vcpus            INTEGER,
		mem_gb           REAL,
		bw_gbps          REAL,
		pps_mpps         REAL,
		enis             INTEGER,
		nitro_gen        TEXT,
		metal            INTEGER,
		agent_version    TEXT,
		isolcpus         TEXT,
		tenancy          TEXT,
		first_seen_unix  INTEGER,
		last_seen_unix   INTEGER
	);

	CREATE INDEX IF NOT EXISTS idx_nodes_ip ON nodes(private_ip);

	CREATE INDEX IF NOT EXISTS idx_m_edge ON measurements(kind, variation, src_ip, dst_ip, unix DESC);
	CREATE INDEX IF NOT EXISTS idx_m_time ON measurements(unix);
	CREATE INDEX IF NOT EXISTS idx_m_run  ON measurements(run_id);
	`
	if _, err := db.Exec(schema); err != nil {
		return err
	}
	if err := migrateAddHopColumns(db); err != nil {
		return err
	}
	return migrateAddColumn(db, "nodes", "tenancy", "TEXT")
}

// migrateAddColumn adds a single column to an existing table if it's not
// already present, using the same PRAGMA table_info check as
// migrateAddHopColumns - generalized since this is now the second column
// added after initial release (see migrateAddHopColumns for why an ALTER is
// needed at all: CREATE TABLE IF NOT EXISTS only shapes a fresh DB).
func migrateAddColumn(db *sql.DB, table, col, sqlType string) error {
	rows, err := db.Query(`PRAGMA table_info(` + table + `)`)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var cid int
		var name, ctype string
		var notnull, pk int
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
			return err
		}
		if name == col {
			return nil // already present
		}
	}
	if err := rows.Err(); err != nil {
		return err
	}
	_, err = db.Exec(`ALTER TABLE ` + table + ` ADD COLUMN ` + col + ` ` + sqlType)
	return err
}

// migrateAddHopColumns adds the hop1/hop2 columns to a pre-existing
// measurements table that predates them. CREATE TABLE IF NOT EXISTS above
// only creates the table with them on a fresh DB; an already-created table on
// a long-running deployment needs an explicit ALTER TABLE. SQLite has no
// "ADD COLUMN IF NOT EXISTS", so this checks pragma table_info first and
// only alters columns that are actually missing - safe to run on every
// startup.
func migrateAddHopColumns(db *sql.DB) error {
	rows, err := db.Query(`PRAGMA table_info(measurements)`)
	if err != nil {
		return err
	}
	existing := map[string]bool{}
	for rows.Next() {
		var cid int
		var name, ctype string
		var notnull, pk int
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
			rows.Close()
			return err
		}
		existing[name] = true
	}
	if err := rows.Err(); err != nil {
		return err
	}
	rows.Close()

	for _, col := range []string{"hop1_p50", "hop1_p99", "hop1_p999", "hop2_p50", "hop2_p99", "hop2_p999"} {
		if existing[col] {
			continue
		}
		if _, err := db.Exec(`ALTER TABLE measurements ADD COLUMN ` + col + ` INTEGER`); err != nil {
			return err
		}
	}
	return nil
}

// UpsertNode records a node's identity, placement and hardware so a measurement
// can be interpreted long after the fleet is gone. Keyed by instance id; every
// re-registration refreshes the row and bumps last_seen. first_seen is kept.
func (s *Store) UpsertNode(n proto.NodeInfo, agentVersion, isolCPUs string, seenUnix int64) error {
	if s == nil {
		return nil // persistence disabled
	}
	if n.InstanceID == "" {
		return nil
	}
	if seenUnix == 0 {
		seenUnix = time.Now().Unix()
	}
	metal := 0
	if n.Metal {
		metal = 1
	}
	_, err := s.db.Exec(`
		INSERT INTO nodes (
			instance_id, private_ip, public_ip, hostname, role, stack,
			region, az, vpc_id, subnet_id, placement_group, pg_strategy,
			instance_type, vcpus, mem_gb, bw_gbps, pps_mpps, enis, nitro_gen, metal,
			agent_version, isolcpus, tenancy, first_seen_unix, last_seen_unix
		) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
		ON CONFLICT(instance_id) DO UPDATE SET
			private_ip=excluded.private_ip, public_ip=excluded.public_ip,
			hostname=excluded.hostname, role=excluded.role, stack=excluded.stack,
			region=excluded.region, az=excluded.az, vpc_id=excluded.vpc_id,
			subnet_id=excluded.subnet_id, placement_group=excluded.placement_group,
			pg_strategy=excluded.pg_strategy, instance_type=excluded.instance_type,
			vcpus=excluded.vcpus, mem_gb=excluded.mem_gb, bw_gbps=excluded.bw_gbps,
			pps_mpps=excluded.pps_mpps, enis=excluded.enis, nitro_gen=excluded.nitro_gen,
			metal=excluded.metal, agent_version=excluded.agent_version,
			isolcpus=excluded.isolcpus, tenancy=excluded.tenancy, last_seen_unix=excluded.last_seen_unix`,
		n.InstanceID, n.PrivateIP, n.PublicIP, n.Hostname, n.Role, n.Stack,
		n.Region, n.AZ, n.VpcID, n.SubnetID, n.PlacementGroup, n.PlacementGroupStrategy,
		n.InstanceType, n.VCPUs, n.MemGB, n.BwGbps, n.PpsMpps, n.ENIs, n.NitroGen, metal,
		agentVersion, isolCPUs, n.Tenancy, seenUnix, seenUnix)
	return err
}

// TouchNode advances last_seen for a node already known, so liveness survives a
// backend restart without a full re-registration.
func (s *Store) TouchNode(instanceID string, seenUnix int64) error {
	if s == nil || instanceID == "" {
		return nil
	}
	_, err := s.db.Exec(`UPDATE nodes SET last_seen_unix=? WHERE instance_id=?`, seenUnix, instanceID)
	return err
}

// InsertRun creates a runs row and returns its id.
func (s *Store) InsertRun(kind, variation, scope, targetIDs string, pairsTotal int, params map[string]any) (int64, error) {
	if s == nil {
		return 0, nil // persistence disabled
	}
	var paramsJSON sql.NullString
	if params != nil {
		b, _ := json.Marshal(params)
		paramsJSON = sql.NullString{String: string(b), Valid: true}
	}
	var scopeN, tidsN sql.NullString
	if scope != "" {
		scopeN = sql.NullString{String: scope, Valid: true}
	}
	if targetIDs != "" {
		tidsN = sql.NullString{String: targetIDs, Valid: true}
	}
	res, err := s.db.Exec(
		`INSERT INTO runs (started_at, kind, variation, scope, target_ids, pairs_total, params)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		time.Now().Unix(), kind, variation, scopeN, tidsN, pairsTotal, paramsJSON)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

// FinishRun sets ended_at and pairs_ok on a run.
func (s *Store) FinishRun(runID int64, pairsOK int) {
	if s == nil || runID <= 0 {
		return
	}
	s.db.Exec("UPDATE runs SET ended_at=?, pairs_ok=? WHERE id=?",
		time.Now().Unix(), pairsOK, runID)
}

// McastReplicatorResult is one (replicator, mode, destination) measurement
// row for the multi-replicator mcast report. Replicator identity/PG/AZ comes
// from the owning run's params JSON (set by RunMcastMatrix), since the
// Telemetry wire message itself only carries src/dst IPs.
type McastReplicatorResult struct {
	RunID         int64   `json:"run_id"`
	StartedAt     int64   `json:"started_at"`
	Mode          string  `json:"mode"`
	ReplicatorID  string  `json:"replicator_id"`
	ReplicatorIP  string  `json:"replicator_ip"`
	ReplicatorPG  string  `json:"replicator_pg"`
	ReplicatorAZ  string  `json:"replicator_az"`
	ReplicatorVPC string  `json:"replicator_vpc"`
	SrcIP         string  `json:"src_ip"`
	DstIP         string  `json:"dst_ip"`
	P50           int64   `json:"p50"`
	P90           int64   `json:"p90"`
	P99           int64   `json:"p99"`
	P999          int64   `json:"p999"`
	Max           int64   `json:"max"`
	LossPct       float64 `json:"loss_pct"`
	Unix          int64   `json:"unix"`
	// Hop1/Hop2: source->replicator and replicator->destination legs of this
	// one-way measurement. nil when the underlying telemetry had no
	// replicator timestamp (see proto.Metrics.Hop1/Hop2).
	Hop1P50 *int64 `json:"hop1_p50,omitempty"`
	Hop1P99 *int64 `json:"hop1_p99,omitempty"`
	Hop1P999 *int64 `json:"hop1_p999,omitempty"`
	Hop2P50 *int64 `json:"hop2_p50,omitempty"`
	Hop2P99 *int64 `json:"hop2_p99,omitempty"`
	Hop2P999 *int64 `json:"hop2_p999,omitempty"`
}

// LatestMcastReplicatorResults returns the most recent measurement per
// (replicator, mode, src, dst) edge, across every replicator swept by the
// last N mcast runs. sinceRunID (0 = no floor) can scope this to a single
// campaign's runs instead of all history — the API handler uses the highest
// run id seen at campaign start for that purpose.
func (s *Store) LatestMcastReplicatorResults(sinceRunID int64, limit int) ([]McastReplicatorResult, error) {
	if s == nil {
		return nil, nil
	}
	if limit <= 0 {
		limit = 500
	}
	rows, err := s.db.Query(`
		SELECT r.id, r.started_at, r.variation, r.params,
		       m.src_ip, m.dst_ip, m.p50, m.p90, m.p99, m.p999, m.max, m.loss_pct, m.unix,
		       m.hop1_p50, m.hop1_p99, m.hop1_p999, m.hop2_p50, m.hop2_p99, m.hop2_p999
		FROM measurements m
		JOIN runs r ON r.id = m.run_id
		WHERE r.kind = 'mcast' AND r.id >= ?
		ORDER BY r.id DESC, m.unix DESC
		LIMIT ?`, sinceRunID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := make([]McastReplicatorResult, 0, limit)
	for rows.Next() {
		var res McastReplicatorResult
		var paramsJSON sql.NullString
		var hop1p50, hop1p99, hop1p999, hop2p50, hop2p99, hop2p999 sql.NullInt64
		if err := rows.Scan(&res.RunID, &res.StartedAt, &res.Mode, &paramsJSON,
			&res.SrcIP, &res.DstIP, &res.P50, &res.P90, &res.P99, &res.P999, &res.Max,
			&res.LossPct, &res.Unix, &hop1p50, &hop1p99, &hop1p999, &hop2p50, &hop2p99, &hop2p999); err != nil {
			return nil, err
		}
		if hop1p50.Valid {
			res.Hop1P50 = &hop1p50.Int64
		}
		if hop1p99.Valid {
			res.Hop1P99 = &hop1p99.Int64
		}
		if hop1p999.Valid {
			res.Hop1P999 = &hop1p999.Int64
		}
		if hop2p50.Valid {
			res.Hop2P50 = &hop2p50.Int64
		}
		if hop2p99.Valid {
			res.Hop2P99 = &hop2p99.Int64
		}
		if hop2p999.Valid {
			res.Hop2P999 = &hop2p999.Int64
		}
		if paramsJSON.Valid {
			var params map[string]any
			if json.Unmarshal([]byte(paramsJSON.String), &params) == nil {
				if v, ok := params["replicator_id"].(string); ok {
					res.ReplicatorID = v
				}
				if v, ok := params["replicator_ip"].(string); ok {
					res.ReplicatorIP = v
				}
				if v, ok := params["replicator_pg"].(string); ok {
					res.ReplicatorPG = v
				}
				if v, ok := params["replicator_az"].(string); ok {
					res.ReplicatorAZ = v
				}
				if v, ok := params["replicator_vpc"].(string); ok {
					res.ReplicatorVPC = v
				}
			}
		}
		out = append(out, res)
	}
	return out, rows.Err()
}

// MeasurementRow is one flat measurement result for the report page, joined
// with its owning run's replicator identity (mcast only) and both endpoints'
// topology (region/az/vpc/pg/role) from the nodes table. This is what
// GET /api/measurements returns — the report builds every table (Latest
// measurements, per-mode heatmaps, All measurements, Per-replicator paths)
// from rows shaped like this instead of the live in-memory matrix, which is
// keyed only by (kind,variation,src,dst) and therefore can only ever hold one
// value per pair — silently collapsing multi-replicator mcast results to
// whichever replicator measured most recently. See
// dev/roadmap/mcast-replicator-selection.md for background.
type MeasurementRow struct {
	RunID     int64   `json:"run_id"`
	Unix      int64   `json:"unix"`
	Kind      string  `json:"kind"`
	Variation string  `json:"variation"`
	SrcIP     string  `json:"src_ip"`
	DstIP     string  `json:"dst_ip"`
	P50       int64   `json:"p50"`
	P90       int64   `json:"p90"`
	P99       int64   `json:"p99"`
	P999      int64   `json:"p999"`
	Max       int64   `json:"max"`
	Min       int64   `json:"min"`
	LossPct   float64 `json:"loss_pct"`
	Messages  int64   `json:"messages"`
	// Replicator identity, decoded from the owning run's params JSON. Empty
	// for ucast (no replicator hop). Callers key/group on ReplicatorIP being
	// "" vs set — that is what actually distinguishes "one value per pair"
	// (ucast) from "one value per (replicator, pair)" (mcast), not Kind alone.
	ReplicatorID  string `json:"replicator_id,omitempty"`
	ReplicatorIP  string `json:"replicator_ip,omitempty"`
	ReplicatorPG  string `json:"replicator_pg,omitempty"`
	ReplicatorAZ  string `json:"replicator_az,omitempty"`
	ReplicatorVPC string `json:"replicator_vpc,omitempty"`
	// Endpoint topology, joined from the nodes table. Best-effort: a node no
	// longer registered under this IP simply leaves these blank rather than
	// failing the row — the measurement itself is still valid.
	SrcRole   string `json:"src_role,omitempty"`
	DstRole   string `json:"dst_role,omitempty"`
	SrcAZ     string `json:"src_az,omitempty"`
	DstAZ     string `json:"dst_az,omitempty"`
	SrcVPC    string `json:"src_vpc,omitempty"`
	DstVPC    string `json:"dst_vpc,omitempty"`
	SrcPG     string `json:"src_pg,omitempty"`
	DstPG     string `json:"dst_pg,omitempty"`
	SrcRegion string `json:"src_region,omitempty"`
	DstRegion string `json:"dst_region,omitempty"`
	SrcTenancy string `json:"src_tenancy,omitempty"`
	DstTenancy string `json:"dst_tenancy,omitempty"`
	// Hop1/Hop2: mcast one-way path split (source->replicator,
	// replicator->destination). nil for ucast and for any mcast result whose
	// telemetry carried no replicator timestamp.
	Hop1P50 *int64 `json:"hop1_p50,omitempty"`
	Hop1P99 *int64 `json:"hop1_p99,omitempty"`
	Hop1P999 *int64 `json:"hop1_p999,omitempty"`
	Hop2P50 *int64 `json:"hop2_p50,omitempty"`
	Hop2P99 *int64 `json:"hop2_p99,omitempty"`
	Hop2P999 *int64 `json:"hop2_p999,omitempty"`
}

// LatestMeasurements returns the most recent measurement per distinct edge,
// where "edge" is (kind, variation, src_ip, dst_ip, replicator_ip). The
// replicator component is what keeps two replicators measuring the same
// destination from collapsing into one row, unlike the live in-memory matrix.
// For ucast, replicator_ip is always "" (no replicator hop), so this
// degenerates to "latest per pair" as expected.
//
// sinceUnix (0 = no floor) bounds the scan by time, so a report over a
// long-lived, heavily-heartbeat-driven fleet doesn't force scanning the full
// retention window before ORDER BY/dedup narrows it — the time-based
// counterpart to LatestMcastReplicatorResults's sinceRunID (ucast runs have no
// natural "campaign" grouping the way a mcast replicator sweep does).
// limit (0 = default 2000) caps rows returned after scoping+dedup. kind ("" =
// both) filters to "ucast" or "mcast".
func (s *Store) LatestMeasurements(kind string, sinceUnix int64, limit int) ([]MeasurementRow, error) {
	if s == nil {
		return nil, nil
	}
	if limit <= 0 {
		limit = 2000
	}
	where := "WHERE m.unix >= ?"
	args := []any{sinceUnix}
	if kind != "" {
		where += " AND r.kind = ?"
		args = append(args, kind)
	}
	// json_extract pulls replicator_ip out of params without a schema change;
	// COALESCE to '' so ucast rows (no params.replicator_ip) key consistently
	// as "no replicator" rather than NULL.
	query := `
		SELECT m.run_id, m.unix, r.kind, m.variation, m.src_ip, m.dst_ip,
		       m.p50, m.p90, m.p99, m.p999, m.max, m.min, m.loss_pct, m.messages,
		       COALESCE(json_extract(r.params, '$.replicator_id'), '') AS replicator_id,
		       COALESCE(json_extract(r.params, '$.replicator_ip'), '') AS replicator_ip,
		       COALESCE(json_extract(r.params, '$.replicator_pg'), '') AS replicator_pg,
		       COALESCE(json_extract(r.params, '$.replicator_az'), '') AS replicator_az,
		       COALESCE(json_extract(r.params, '$.replicator_vpc'), '') AS replicator_vpc,
		       m.hop1_p50, m.hop1_p99, m.hop1_p999, m.hop2_p50, m.hop2_p99, m.hop2_p999
		FROM measurements m
		JOIN runs r ON r.id = m.run_id
		` + where + `
		ORDER BY m.unix DESC, m.id DESC`
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	// One pass; ORDER BY above means the first row seen per key is the newest.
	seen := map[string]bool{}
	out := make([]MeasurementRow, 0, limit)
	for rows.Next() {
		var r MeasurementRow
		var p90, p99, p999, max, min, messages sql.NullInt64
		var hop1p50, hop1p99, hop1p999, hop2p50, hop2p99, hop2p999 sql.NullInt64
		if err := rows.Scan(&r.RunID, &r.Unix, &r.Kind, &r.Variation, &r.SrcIP, &r.DstIP,
			&r.P50, &p90, &p99, &p999, &max, &min, &r.LossPct, &messages,
			&r.ReplicatorID, &r.ReplicatorIP, &r.ReplicatorPG, &r.ReplicatorAZ, &r.ReplicatorVPC,
			&hop1p50, &hop1p99, &hop1p999, &hop2p50, &hop2p99, &hop2p999); err != nil {
			return nil, err
		}
		r.P90, r.P99, r.P999, r.Max, r.Min, r.Messages = p90.Int64, p99.Int64, p999.Int64, max.Int64, min.Int64, messages.Int64
		if hop1p50.Valid {
			r.Hop1P50 = &hop1p50.Int64
		}
		if hop1p99.Valid {
			r.Hop1P99 = &hop1p99.Int64
		}
		if hop1p999.Valid {
			r.Hop1P999 = &hop1p999.Int64
		}
		if hop2p50.Valid {
			r.Hop2P50 = &hop2p50.Int64
		}
		if hop2p99.Valid {
			r.Hop2P99 = &hop2p99.Int64
		}
		if hop2p999.Valid {
			r.Hop2P999 = &hop2p999.Int64
		}
		key := r.Variation + "|" + r.SrcIP + "|" + r.DstIP + "|" + r.ReplicatorIP
		if seen[key] {
			continue
		}
		seen[key] = true
		out = append(out, r)
		if len(out) >= limit {
			break
		}
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}

	// Best-effort topology enrichment: one query for every distinct IP
	// involved, rather than N+1 per-row lookups.
	ips := map[string]bool{}
	for _, r := range out {
		ips[r.SrcIP] = true
		ips[r.DstIP] = true
	}
	topo, err := s.nodeTopologyByIP(ips)
	if err != nil {
		return nil, err
	}
	for i := range out {
		if t, ok := topo[out[i].SrcIP]; ok {
			out[i].SrcRole, out[i].SrcAZ, out[i].SrcVPC, out[i].SrcPG, out[i].SrcRegion, out[i].SrcTenancy =
				t.role, t.az, t.vpc, t.pg, t.region, t.tenancy
		}
		if t, ok := topo[out[i].DstIP]; ok {
			out[i].DstRole, out[i].DstAZ, out[i].DstVPC, out[i].DstPG, out[i].DstRegion, out[i].DstTenancy =
				t.role, t.az, t.vpc, t.pg, t.region, t.tenancy
		}
	}
	return out, nil
}

type nodeTopo struct {
	role, az, vpc, pg, region, tenancy string
}

// nodeTopologyByIP looks up topology for a set of private IPs in one query.
func (s *Store) nodeTopologyByIP(ips map[string]bool) (map[string]nodeTopo, error) {
	out := map[string]nodeTopo{}
	if len(ips) == 0 {
		return out, nil
	}
	placeholders := make([]string, 0, len(ips))
	args := make([]any, 0, len(ips))
	for ip := range ips {
		placeholders = append(placeholders, "?")
		args = append(args, ip)
	}
	q := "SELECT private_ip, role, az, vpc_id, placement_group, region, tenancy FROM nodes WHERE private_ip IN (" +
		strings.Join(placeholders, ",") + ")"
	rows, err := s.db.Query(q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var ip string
		var role, az, vpc, pg, region, tenancy sql.NullString
		if err := rows.Scan(&ip, &role, &az, &vpc, &pg, &region, &tenancy); err != nil {
			return nil, err
		}
		out[ip] = nodeTopo{role: role.String, az: az.String, vpc: vpc.String, pg: pg.String, region: region.String, tenancy: tenancy.String}
	}
	return out, rows.Err()
}

// writer drains the channel, batching into transactions.
func (s *Store) writer() {
	defer s.wg.Done()
	batch := make([]measurementRow, 0, batchSize)
	timer := time.NewTimer(batchTimeout)
	defer timer.Stop()

	flush := func() {
		if len(batch) == 0 {
			return
		}
		tx, err := s.db.Begin()
		if err != nil {
			log.Printf("store: begin tx: %v", err)
			batch = batch[:0]
			return
		}
		stmt, err := tx.Prepare(
			`INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip, tx_mode,
			  p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct, cmd_id,
			  hop1_p50, hop1_p99, hop1_p999, hop2_p50, hop2_p99, hop2_p999)
			 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`)
		if err != nil {
			tx.Rollback()
			log.Printf("store: prepare: %v", err)
			batch = batch[:0]
			return
		}
		for _, r := range batch {
			var runID sql.NullInt64
			if r.RunID > 0 {
				runID = sql.NullInt64{Int64: r.RunID, Valid: true}
			}
			nullOf := func(p *int64) sql.NullInt64 {
				if p == nil {
					return sql.NullInt64{}
				}
				return sql.NullInt64{Int64: *p, Valid: true}
			}
			stmt.Exec(runID, r.Unix, r.Kind, r.Variation, r.SrcIP, r.DstIP, r.TxMode,
				r.P50, r.P90, r.P99, r.P999, r.Max, r.Min, r.Mean,
				r.Messages, r.Lost, r.LossPct, r.CmdID,
				nullOf(r.Hop1P50), nullOf(r.Hop1P99), nullOf(r.Hop1P999),
				nullOf(r.Hop2P50), nullOf(r.Hop2P99), nullOf(r.Hop2P999))
		}
		stmt.Close()
		if err := tx.Commit(); err != nil {
			log.Printf("store: commit: %v", err)
		}
		batch = batch[:0]
	}

	for {
		select {
		case <-s.stopCh:
			// Drain remaining.
		drain:
			for {
				select {
				case r := <-s.ch:
					batch = append(batch, r)
					if len(batch) >= batchSize {
						flush()
					}
				default:
					break drain
				}
			}
			flush()
			return
		case r := <-s.ch:
			batch = append(batch, r)
			if len(batch) >= batchSize {
				flush()
				if !timer.Stop() {
					select {
					case <-timer.C:
					default:
					}
				}
				timer.Reset(batchTimeout)
			}
		case ack := <-s.flushReq:
			// Drain whatever is queued, commit it, then acknowledge.
		pending:
			for {
				select {
				case r := <-s.ch:
					batch = append(batch, r)
				default:
					break pending
				}
			}
			flush()
			close(ack)
		case <-timer.C:
			flush()
			timer.Reset(batchTimeout)
		}
	}
}

// stopWriter stops the writer goroutine (used in tests to simulate stall).
func (s *Store) stopWriter() {
	close(s.stopCh)
	s.wg.Wait()
	// Reopen stop channel so Close doesn't double-close.
	s.stopCh = make(chan struct{})
}

// Close flushes pending writes and closes the database.
func (s *Store) Close() {
	select {
	case <-s.stopCh:
		// Already stopped.
	default:
		close(s.stopCh)
	}
	s.wg.Wait()
	s.db.Close()
}

// RecordMeasurement non-blocking sends a measurement to the store channel.
// Safe with nil store (persistence disabled).
func RecordMeasurement(s *Store, t proto.Telemetry, runID int64) {
	if s == nil {
		return
	}
	row := measurementRow{
		RunID:     runID,
		Unix:      t.Unix,
		Kind:      t.Kind,
		Variation: t.Variation,
		SrcIP:     t.SrcIP,
		DstIP:     t.DstIP,
		TxMode:    t.TxMode,
		P50:       t.Metrics.ServiceRTT.P50,
		P90:       t.Metrics.ServiceRTT.P90,
		P99:       t.Metrics.ServiceRTT.P99,
		P999:      t.Metrics.ServiceRTT.P999,
		Max:       t.Metrics.ServiceRTT.Max,
		Min:       t.Metrics.ServiceRTT.Min,
		Mean:      t.Metrics.ServiceRTT.Mean,
		Messages:  t.Metrics.Messages,
		Lost:      t.Metrics.Lost,
		LossPct:   t.Metrics.LossPct,
		CmdID:     t.CmdID,
	}
	if t.Metrics.Hop1 != nil {
		row.Hop1P50, row.Hop1P99, row.Hop1P999 = &t.Metrics.Hop1.P50, &t.Metrics.Hop1.P99, &t.Metrics.Hop1.P999
	}
	if t.Metrics.Hop2 != nil {
		row.Hop2P50, row.Hop2P99, row.Hop2P999 = &t.Metrics.Hop2.P50, &t.Metrics.Hop2.P99, &t.Metrics.Hop2.P999
	}
	select {
	case s.ch <- row:
	default:
		dropped := atomic.AddInt64(&s.Dropped, 1)
		now := time.Now().Unix()
		last := s.lastDropLog.Load()
		if now-last >= 60 {
			if s.lastDropLog.CompareAndSwap(last, now) {
				log.Printf("store: dropped %d measurements (channel full)", dropped)
			}
		}
	}
}

// SeedCollector repopulates the in-memory rings from the newest perEdge
// samples per edge so a restart does not blank the map.
func (s *Store) SeedCollector(c *collector.Collector, perEdge int) error {
	rows, err := s.db.Query(`
		SELECT kind, variation, src_ip, dst_ip, unix, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct, tx_mode
		FROM (
			SELECT *, ROW_NUMBER() OVER (
				PARTITION BY kind, variation, src_ip, dst_ip ORDER BY unix DESC
			) AS rn
			FROM measurements
		) WHERE rn <= ?
		ORDER BY unix ASC`, perEdge)
	if err != nil {
		return err
	}
	defer rows.Close()

	for rows.Next() {
		var kind, variation, srcIP, dstIP string
		var txMode sql.NullString
		var unix, p50, p90, p99, p999, max, min, mean, messages, lost int64
		var lossPct float64
		if err := rows.Scan(&kind, &variation, &srcIP, &dstIP, &unix,
			&p50, &p90, &p99, &p999, &max, &min, &mean,
			&messages, &lost, &lossPct, &txMode); err != nil {
			return err
		}
		t := proto.Telemetry{
			Kind:      kind,
			Variation: variation,
			SrcIP:     srcIP,
			DstIP:     dstIP,
			Unix:      unix,
			TxMode:    txMode.String,
			Metrics: proto.Metrics{
				ServiceRTT: proto.Pct{
					P50: p50, P90: p90, P99: p99, P999: p999,
					Max: max, Min: min, Mean: mean,
				},
				Messages: messages,
				Lost:     lost,
				LossPct:  lossPct,
			},
		}
		c.Apply(t)
	}
	return rows.Err()
}

// seedFromStore is a nil-safe wrapper for SeedCollector.
func seedFromStore(s *Store, c *collector.Collector, perEdge int) error {
	if s == nil {
		return nil
	}
	return s.SeedCollector(c, perEdge)
}

// runRetention deletes old measurements and orphaned runs.
func (s *Store) runRetention(retentionDays int) {
	cutoff := time.Now().Unix() - int64(retentionDays)*24*3600
	_, err := s.db.Exec("DELETE FROM measurements WHERE unix < ?", cutoff)
	if err != nil {
		log.Printf("store: retention delete: %v", err)
		return
	}
	// Delete orphaned runs (no remaining measurements).
	s.db.Exec("DELETE FROM runs WHERE id NOT IN (SELECT DISTINCT run_id FROM measurements WHERE run_id IS NOT NULL)")
}

// retentionLoop runs hourly deletion + weekly VACUUM.
func (s *Store) retentionLoop(retentionDays int) {
	defer s.wg.Done()
	hourly := time.NewTicker(1 * time.Hour)
	defer hourly.Stop()
	vacuumCount := 0
	for {
		select {
		case <-s.stopCh:
			return
		case <-hourly.C:
			s.runRetention(retentionDays)
			vacuumCount++
			if vacuumCount%168 == 0 { // ~weekly (168 hours)
				s.db.Exec("VACUUM")
			}
		}
	}
}
