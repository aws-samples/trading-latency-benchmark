package store

import (
	"database/sql"
	"encoding/json"
	"log"
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

	// flushed is signalled by the writer after each batch commit so tests and
	// shutdown can wait for durability instead of sleeping.
	flushMu sync.Mutex
	flushCh chan struct{}
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
// Without this, a caller can only sleep and hope, which makes tests flaky and
// gives shutdown no way to guarantee the last batch reached disk.
func (s *Store) Flush() {
	if s == nil {
		return
	}
	s.flushMu.Lock()
	if s.flushCh == nil {
		s.flushCh = make(chan struct{}, 1)
	}
	ch := s.flushCh
	s.flushMu.Unlock()
	// Drain any stale signal, then wait for the next commit.
	select {
	case <-ch:
	default:
	}
	deadline := time.After(5 * time.Second)
	for {
		if len(s.ch) == 0 {
			// Queue drained; one more commit boundary guarantees it is on disk.
			select {
			case <-ch:
			case <-time.After(batchTimeout + 200*time.Millisecond):
			case <-deadline:
			}
			return
		}
		select {
		case <-ch:
		case <-time.After(50 * time.Millisecond):
		case <-deadline:
			return
		}
	}
}

// signalFlush notifies any waiting Flush that a batch was committed.
func (s *Store) signalFlush() {
	s.flushMu.Lock()
	if s.flushCh == nil {
		s.flushCh = make(chan struct{}, 1)
	}
	ch := s.flushCh
	s.flushMu.Unlock()
	select {
	case ch <- struct{}{}:
	default:
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
		db:     db,
		ch:     make(chan measurementRow, storeChanCap),
		stopCh: make(chan struct{}),
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
		cmd_id    TEXT
	);

	CREATE INDEX IF NOT EXISTS idx_m_edge ON measurements(kind, variation, src_ip, dst_ip, unix DESC);
	CREATE INDEX IF NOT EXISTS idx_m_time ON measurements(unix);
	CREATE INDEX IF NOT EXISTS idx_m_run  ON measurements(run_id);
	`
	_, err := db.Exec(schema)
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
			  p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct, cmd_id)
			 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`)
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
			stmt.Exec(runID, r.Unix, r.Kind, r.Variation, r.SrcIP, r.DstIP, r.TxMode,
				r.P50, r.P90, r.P99, r.P999, r.Max, r.Min, r.Mean,
				r.Messages, r.Lost, r.LossPct, r.CmdID)
		}
		stmt.Close()
		if err := tx.Commit(); err != nil {
			log.Printf("store: commit: %v", err)
		}
		batch = batch[:0]
		s.signalFlush()
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
