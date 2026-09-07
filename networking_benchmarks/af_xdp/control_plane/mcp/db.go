// Package mcp implements a read-only MCP (Model Context Protocol) server
// over JSON-RPC 2.0 stdio for querying AF_XDP latency measurements stored
// in SQLite. The database is opened in read-only mode to guarantee the
// analysis surface cannot mutate measurement data.
package mcp

import (
	"database/sql"
	"fmt"
	"net/url"
	"os"
	"strings"

	_ "modernc.org/sqlite"
)

// DB wraps a read-only SQLite connection.
type DB struct {
	conn *sql.DB
}

// OpenDB opens the SQLite file read-only. Returns an error if the file does not
// exist or is inaccessible.
//
// Read-only is enforced in the DSN (mode=ro), which applies to every connection
// database/sql opens. A PRAGMA issued after sql.Open would not: it lands on
// whichever pooled connection served the call, leaving any later connection
// writable, and a caller could switch it back off with query_only=OFF. The
// _pragma parameter is defence in depth -- unlike a bare Exec it is replayed on
// each new connection by the driver.
func OpenDB(path string) (*DB, error) {
	if _, err := os.Stat(path); err != nil {
		return nil, fmt.Errorf("database file not accessible: %w", err)
	}
	// file: URI so mode=ro is parsed as a parameter rather than part of the name.
	dsn := "file:" + url.PathEscape(path) + "?mode=ro&_pragma=query_only(1)"
	conn, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open database: %w", err)
	}
	if err := conn.Ping(); err != nil {
		conn.Close()
		return nil, fmt.Errorf("ping database: %w", err)
	}
	conn.SetMaxOpenConns(4)
	return &DB{conn: conn}, nil
}

// Close closes the database connection.
func (db *DB) Close() {
	if db != nil && db.conn != nil {
		db.conn.Close()
	}
}

// ToolResult is the envelope returned by every tool - rows plus the SQL.
type ToolResult struct {
	Rows []any  `json:"rows"`
	SQL  string `json:"sql"`
}

// --- list_runs ---

// ListRunsParams are the optional filters for list_runs.
type ListRunsParams struct {
	Kind      string `json:"kind"`
	Variation string `json:"variation"`
	Since     int64  `json:"since"`
	Limit     int    `json:"limit"`
}

// ListRuns returns matching campaign runs.
func (db *DB) ListRuns(p ListRunsParams) (ToolResult, error) {
	var clauses []string
	var args []any

	if p.Kind != "" {
		clauses = append(clauses, "kind = ?")
		args = append(args, p.Kind)
	}
	if p.Variation != "" {
		clauses = append(clauses, "variation = ?")
		args = append(args, p.Variation)
	}
	if p.Since > 0 {
		clauses = append(clauses, "started_at >= ?")
		args = append(args, p.Since)
	}

	q := "SELECT id, started_at, ended_at, kind, variation, scope, target_ids, pairs_total, pairs_ok, params FROM runs"
	if len(clauses) > 0 {
		q += " WHERE " + strings.Join(clauses, " AND ")
	}
	q += " ORDER BY started_at DESC"

	limit := p.Limit
	if limit <= 0 {
		limit = 100
	}
	q += " LIMIT ?"
	args = append(args, limit)

	rows, err := db.conn.Query(q, args...)
	if err != nil {
		return ToolResult{SQL: q}, fmt.Errorf("list_runs query: %w", err)
	}
	defer rows.Close()

	var results []any
	for rows.Next() {
		var id, startedAt int64
		var endedAt sql.NullInt64
		var kind, variation string
		var scope, targetIDs, params sql.NullString
		var pairsTotal, pairsOK sql.NullInt64

		if err := rows.Scan(&id, &startedAt, &endedAt, &kind, &variation, &scope, &targetIDs, &pairsTotal, &pairsOK, &params); err != nil {
			return ToolResult{SQL: q}, fmt.Errorf("scan: %w", err)
		}
		row := map[string]any{
			"id":          id,
			"started_at":  startedAt,
			"kind":        kind,
			"variation":   variation,
			"pairs_total": nullInt(pairsTotal),
			"pairs_ok":    nullInt(pairsOK),
		}
		if endedAt.Valid {
			row["ended_at"] = endedAt.Int64
		}
		if scope.Valid {
			row["scope"] = scope.String
		}
		if targetIDs.Valid {
			row["target_ids"] = targetIDs.String
		}
		if params.Valid {
			row["params"] = params.String
		}
		results = append(results, row)
	}
	if results == nil {
		results = []any{}
	}
	return ToolResult{Rows: results, SQL: q}, rows.Err()
}

// --- query_latency ---

// QueryLatencyParams are the optional filters for query_latency.
type QueryLatencyParams struct {
	Src       string `json:"src"`
	Dst       string `json:"dst"`
	Kind      string `json:"kind"`
	Variation string `json:"variation"`
	Since     int64  `json:"since"`
	Limit     int    `json:"limit"`
}

// QueryLatency returns per-pair measurement history.
func (db *DB) QueryLatency(p QueryLatencyParams) (ToolResult, error) {
	var clauses []string
	var args []any

	if p.Src != "" {
		clauses = append(clauses, "src_ip = ?")
		args = append(args, p.Src)
	}
	if p.Dst != "" {
		clauses = append(clauses, "dst_ip = ?")
		args = append(args, p.Dst)
	}
	if p.Kind != "" {
		clauses = append(clauses, "kind = ?")
		args = append(args, p.Kind)
	}
	if p.Variation != "" {
		clauses = append(clauses, "variation = ?")
		args = append(args, p.Variation)
	}
	if p.Since > 0 {
		clauses = append(clauses, "unix >= ?")
		args = append(args, p.Since)
	}

	q := "SELECT run_id, unix, kind, variation, src_ip, dst_ip, tx_mode, p50, p90, p99, p999, max, min, mean, messages, lost, loss_pct FROM measurements"
	if len(clauses) > 0 {
		q += " WHERE " + strings.Join(clauses, " AND ")
	}
	q += " ORDER BY unix DESC"

	limit := p.Limit
	if limit <= 0 {
		limit = 100
	}
	q += " LIMIT ?"
	args = append(args, limit)

	rows, err := db.conn.Query(q, args...)
	if err != nil {
		return ToolResult{SQL: q}, fmt.Errorf("query_latency: %w", err)
	}
	defer rows.Close()

	var results []any
	for rows.Next() {
		var runID sql.NullInt64
		var unix int64
		var kind, variation, srcIP, dstIP string
		var txMode sql.NullString
		var p50, p90, p99, p999, max, min, mean sql.NullInt64
		var messages, lost sql.NullInt64
		var lossPct sql.NullFloat64

		if err := rows.Scan(&runID, &unix, &kind, &variation, &srcIP, &dstIP, &txMode,
			&p50, &p90, &p99, &p999, &max, &min, &mean, &messages, &lost, &lossPct); err != nil {
			return ToolResult{SQL: q}, fmt.Errorf("scan: %w", err)
		}
		row := map[string]any{
			"unix":      unix,
			"kind":      kind,
			"variation": variation,
			"src_ip":    srcIP,
			"dst_ip":    dstIP,
			"p50":       nullInt(p50),
			"p90":       nullInt(p90),
			"p99":       nullInt(p99),
			"p999":      nullInt(p999),
			"max":       nullInt(max),
			"min":       nullInt(min),
			"mean":      nullInt(mean),
			"messages":  nullInt(messages),
			"lost":      nullInt(lost),
			"loss_pct":  nullFloat(lossPct),
		}
		if runID.Valid {
			row["run_id"] = runID.Int64
		}
		if txMode.Valid {
			row["tx_mode"] = txMode.String
		}
		results = append(results, row)
	}
	if results == nil {
		results = []any{}
	}
	return ToolResult{Rows: results, SQL: q}, rows.Err()
}

// --- compare_runs ---

// CompareRuns computes per-cell p50 delta between two runs.
func (db *DB) CompareRuns(runA, runB int64) (ToolResult, error) {
	// Pair on the EDGE, not on variation. Two runs are frequently different
	// variations (kernel baseline vs xdp), and that is the main reason to compare
	// two campaigns at all -- joining on variation would return nothing there.
	// Each side's variation is returned so the caller can see whether a delta is
	// a change over time or a difference between datapaths.
	q := `SELECT a.src_ip, a.dst_ip,
		a.variation AS variation_a, b.variation AS variation_b,
		a.p50 AS p50_a, b.p50 AS p50_b, (b.p50 - a.p50) AS delta_p50,
		a.p99 AS p99_a, b.p99 AS p99_b, (b.p99 - a.p99) AS delta_p99
	FROM measurements a
	INNER JOIN measurements b
		ON a.src_ip = b.src_ip AND a.dst_ip = b.dst_ip AND a.kind = b.kind
	WHERE a.run_id = ? AND b.run_id = ?
	ORDER BY a.src_ip, a.dst_ip`

	rows, err := db.conn.Query(q, runA, runB)
	if err != nil {
		return ToolResult{SQL: q}, fmt.Errorf("compare_runs: %w", err)
	}
	defer rows.Close()

	var results []any
	for rows.Next() {
		var srcIP, dstIP, varA, varB string
		var p50A, p50B, deltaP50, p99A, p99B, deltaP99 int64
		if err := rows.Scan(&srcIP, &dstIP, &varA, &varB,
			&p50A, &p50B, &deltaP50, &p99A, &p99B, &deltaP99); err != nil {
			return ToolResult{SQL: q}, fmt.Errorf("scan: %w", err)
		}
		results = append(results, map[string]any{
			"src_ip":      srcIP,
			"dst_ip":      dstIP,
			"variation_a": varA,
			"variation_b": varB,
			"p50_a":       p50A,
			"p50_b":       p50B,
			"delta_p50":   deltaP50,
			"p99_a":       p99A,
			"p99_b":       p99B,
			"delta_p99":   deltaP99,
		})
	}
	if results == nil {
		results = []any{}
	}
	return ToolResult{Rows: results, SQL: q}, rows.Err()
}

// --- compare_modes ---

// CompareModes compares the newest measurement of two variations per edge.
func (db *DB) CompareModes(kind, variationA, variationB string, since int64) (ToolResult, error) {
	// Get newest measurement per edge for each variation using window functions.
	q := `WITH latest_a AS (
		SELECT src_ip, dst_ip, p50, p99, unix,
			ROW_NUMBER() OVER (PARTITION BY src_ip, dst_ip ORDER BY unix DESC) AS rn
		FROM measurements WHERE kind = ? AND variation = ?` +
		sinceSuffix(since, 3) + `
	), latest_b AS (
		SELECT src_ip, dst_ip, p50, p99, unix,
			ROW_NUMBER() OVER (PARTITION BY src_ip, dst_ip ORDER BY unix DESC) AS rn
		FROM measurements WHERE kind = ? AND variation = ?` +
		sinceSuffix(since, 5) + `
	)
	SELECT a.src_ip, a.dst_ip,
		a.p50 AS p50_a, b.p50 AS p50_b, (b.p50 - a.p50) AS delta_p50,
		a.p99 AS p99_a, b.p99 AS p99_b, (b.p99 - a.p99) AS delta_p99
	FROM latest_a a
	INNER JOIN latest_b b ON a.src_ip = b.src_ip AND a.dst_ip = b.dst_ip
	WHERE a.rn = 1 AND b.rn = 1
	ORDER BY a.src_ip, a.dst_ip`

	args := []any{kind, variationA}
	if since > 0 {
		args = append(args, since)
	}
	args = append(args, kind, variationB)
	if since > 0 {
		args = append(args, since)
	}

	rows, err := db.conn.Query(q, args...)
	if err != nil {
		return ToolResult{SQL: q}, fmt.Errorf("compare_modes: %w", err)
	}
	defer rows.Close()

	var results []any
	for rows.Next() {
		var srcIP, dstIP string
		var p50A, p50B, deltaP50, p99A, p99B, deltaP99 int64
		if err := rows.Scan(&srcIP, &dstIP, &p50A, &p50B, &deltaP50, &p99A, &p99B, &deltaP99); err != nil {
			return ToolResult{SQL: q}, fmt.Errorf("scan: %w", err)
		}
		results = append(results, map[string]any{
			"src_ip":            srcIP,
			"dst_ip":            dstIP,
			"p50_" + variationA: p50A,
			"p50_" + variationB: p50B,
			"delta_p50":         deltaP50,
			"p99_" + variationA: p99A,
			"p99_" + variationB: p99B,
			"delta_p99":         deltaP99,
		})
	}
	if results == nil {
		results = []any{}
	}
	return ToolResult{Rows: results, SQL: q}, rows.Err()
}

// sinceSuffix appends an AND unix >= ? clause when since > 0.
// argIdx is used only for documentation; the actual param binding is positional.
func sinceSuffix(since int64, _ int) string {
	if since > 0 {
		return " AND unix >= ?"
	}
	return ""
}

// --- regressions ---

// RegressionsParams are the filters for regression detection.
type RegressionsParams struct {
	ThresholdUs int64  `json:"threshold_us"`
	WindowHours int64  `json:"window_hours"`
	Kind        string `json:"kind"`
	Variation   string `json:"variation"`
}

// Regressions finds pairs whose p50 grew by more than threshold_us within
// the window, comparing oldest vs newest measurement.
func (db *DB) Regressions(p RegressionsParams) (ToolResult, error) {
	var extraClauses []string
	var args []any

	if p.WindowHours > 0 {
		// Compute cutoff from current unix time is not ideal for tests with
		// old timestamps. Use a subquery on the data range instead.
		// Actually, to be testable, use a window from the max timestamp in the DB.
		extraClauses = append(extraClauses, "unix >= (SELECT MAX(unix) FROM measurements) - ?")
		args = append(args, p.WindowHours*3600)
	}
	if p.Kind != "" {
		extraClauses = append(extraClauses, "kind = ?")
		args = append(args, p.Kind)
	}
	if p.Variation != "" {
		extraClauses = append(extraClauses, "variation = ?")
		args = append(args, p.Variation)
	}

	where := ""
	if len(extraClauses) > 0 {
		where = " WHERE " + strings.Join(extraClauses, " AND ")
	}

	// Find oldest and newest p50 per edge within the window, compute delta.
	q := `WITH windowed AS (
		SELECT kind, variation, src_ip, dst_ip, p50, unix,
			ROW_NUMBER() OVER (PARTITION BY kind, variation, src_ip, dst_ip ORDER BY unix ASC) AS rn_oldest,
			ROW_NUMBER() OVER (PARTITION BY kind, variation, src_ip, dst_ip ORDER BY unix DESC) AS rn_newest
		FROM measurements` + where + `
	)
	SELECT o.kind, o.variation, o.src_ip, o.dst_ip,
		o.p50 AS p50_old, n.p50 AS p50_new, (n.p50 - o.p50) AS delta_p50
	FROM windowed o
	INNER JOIN windowed n ON o.kind = n.kind AND o.variation = n.variation
		AND o.src_ip = n.src_ip AND o.dst_ip = n.dst_ip
	WHERE o.rn_oldest = 1 AND n.rn_newest = 1 AND (n.p50 - o.p50) > ?
	ORDER BY (n.p50 - o.p50) DESC`

	args = append(args, p.ThresholdUs)

	rows, err := db.conn.Query(q, args...)
	if err != nil {
		return ToolResult{SQL: q}, fmt.Errorf("regressions: %w", err)
	}
	defer rows.Close()

	var results []any
	for rows.Next() {
		var kind, variation, srcIP, dstIP string
		var p50Old, p50New, deltaP50 int64
		if err := rows.Scan(&kind, &variation, &srcIP, &dstIP, &p50Old, &p50New, &deltaP50); err != nil {
			return ToolResult{SQL: q}, fmt.Errorf("scan: %w", err)
		}
		results = append(results, map[string]any{
			"kind":      kind,
			"variation": variation,
			"src_ip":    srcIP,
			"dst_ip":    dstIP,
			"p50_old":   p50Old,
			"p50_new":   p50New,
			"delta_p50": deltaP50,
		})
	}
	if results == nil {
		results = []any{}
	}
	return ToolResult{Rows: results, SQL: q}, rows.Err()
}

// --- topology_summary ---

// TopologySummary returns the newest measurement per edge across all kinds.
// ListNodesParams filters the fleet inventory.
type ListNodesParams struct {
	InstanceID  string `json:"instance_id"`
	PrivateIP   string `json:"private_ip"`
	Role        string `json:"role"`
	AZ          string `json:"az"`
	Region      string `json:"region"`
	PGStrategy  string `json:"pg_strategy"`
	Limit       int    `json:"limit"`
}

// hasNodes reports whether this database carries the fleet inventory. Databases
// written before nodes were recorded still answer every other tool, so the
// metadata joins degrade instead of failing.
func (db *DB) hasNodes() bool {
	var n int
	err := db.conn.QueryRow(
		`SELECT count(*) FROM sqlite_master WHERE type='table' AND name='nodes'`).Scan(&n)
	return err == nil && n > 0
}

// ListNodes returns the recorded identity, placement and hardware of each node.
func (db *DB) ListNodes(p ListNodesParams) (ToolResult, error) {
	if !db.hasNodes() {
		return ToolResult{Rows: []any{}, SQL: "nodes table absent in this database"}, nil
	}
	var clauses []string
	var args []any
	for _, f := range []struct {
		col string
		val string
	}{
		{"instance_id", p.InstanceID}, {"private_ip", p.PrivateIP}, {"role", p.Role},
		{"az", p.AZ}, {"region", p.Region}, {"pg_strategy", p.PGStrategy},
	} {
		if f.val != "" {
			clauses = append(clauses, f.col+" = ?")
			args = append(args, f.val)
		}
	}
	q := `SELECT instance_id, private_ip, public_ip, hostname, role, stack,
		region, az, vpc_id, subnet_id, placement_group, pg_strategy,
		instance_type, vcpus, mem_gb, bw_gbps, enis, nitro_gen, metal,
		agent_version, isolcpus, first_seen_unix, last_seen_unix FROM nodes`
	if len(clauses) > 0 {
		q += " WHERE " + strings.Join(clauses, " AND ")
	}
	q += " ORDER BY region, az, private_ip"
	if p.Limit <= 0 || p.Limit > 1000 {
		p.Limit = 200
	}
	q += " LIMIT ?"
	args = append(args, p.Limit)

	rows, err := db.conn.Query(q, args...)
	if err != nil {
		return ToolResult{SQL: q}, fmt.Errorf("list_nodes: %w", err)
	}
	defer rows.Close()

	var out []any
	for rows.Next() {
		var instanceID, privateIP string
		var publicIP, hostname, role, stack, region, az, vpcID, subnetID sql.NullString
		var pg, pgStrategy, instType, nitroGen, agentVer, isolCPUs sql.NullString
		var vcpus, enis, metal, firstSeen, lastSeen sql.NullInt64
		var memGB, bwGbps sql.NullFloat64
		if err := rows.Scan(&instanceID, &privateIP, &publicIP, &hostname, &role, &stack,
			&region, &az, &vpcID, &subnetID, &pg, &pgStrategy,
			&instType, &vcpus, &memGB, &bwGbps, &enis, &nitroGen, &metal,
			&agentVer, &isolCPUs, &firstSeen, &lastSeen); err != nil {
			return ToolResult{SQL: q}, fmt.Errorf("list_nodes scan: %w", err)
		}
		out = append(out, map[string]any{
			"instance_id": instanceID, "private_ip": privateIP,
			"public_ip": nullStr(publicIP), "hostname": nullStr(hostname),
			"role": nullStr(role), "stack": nullStr(stack),
			"region": nullStr(region), "az": nullStr(az),
			"vpc_id": nullStr(vpcID), "subnet_id": nullStr(subnetID),
			"placement_group": nullStr(pg), "pg_strategy": nullStr(pgStrategy),
			"instance_type": nullStr(instType), "vcpus": nullInt(vcpus),
			"mem_gb": nullFloat(memGB), "bw_gbps": nullFloat(bwGbps),
			"enis": nullInt(enis), "nitro_gen": nullStr(nitroGen),
			"metal": nullInt(metal), "agent_version": nullStr(agentVer),
			"isolcpus": nullStr(isolCPUs),
			"first_seen_unix": nullInt(firstSeen), "last_seen_unix": nullInt(lastSeen),
		})
	}
	if out == nil {
		out = []any{}
	}
	return ToolResult{Rows: out, SQL: q}, rows.Err()
}

func nullStr(n sql.NullString) any {
	if !n.Valid {
		return nil
	}
	return n.String
}

func (db *DB) TopologySummary() (ToolResult, error) {
	// Each edge carries its endpoints' placement so a latency figure can be read
	// without a second lookup: AZ, VPC, subnet, placement group and its strategy
	// are what explain the difference between 23us and 12ms.
	meta := db.hasNodes()
	sel := `SELECT m.kind, m.variation, m.src_ip, m.dst_ip, m.unix,
		m.p50, m.p90, m.p99, m.p999, m.messages, m.lost, m.loss_pct`
	join := ""
	if meta {
		sel += `, s.instance_id, s.role, s.az, s.vpc_id, s.subnet_id,
			s.placement_group, s.pg_strategy, s.instance_type,
			d.instance_id, d.role, d.az, d.vpc_id, d.subnet_id,
			d.placement_group, d.pg_strategy, d.instance_type`
		join = ` LEFT JOIN nodes s ON s.private_ip = m.src_ip
			LEFT JOIN nodes d ON d.private_ip = m.dst_ip`
	}
	q := sel + ` FROM (
		SELECT *, ROW_NUMBER() OVER (
			PARTITION BY kind, variation, src_ip, dst_ip ORDER BY unix DESC
		) AS rn
		FROM measurements
	) m` + join + `
	WHERE m.rn = 1
	ORDER BY m.kind, m.variation, m.src_ip, m.dst_ip`

	rows, err := db.conn.Query(q)
	if err != nil {
		return ToolResult{SQL: q}, fmt.Errorf("topology_summary: %w", err)
	}
	defer rows.Close()

	var results []any
	for rows.Next() {
		var kind, variation, srcIP, dstIP string
		var unix, p50 int64
		var p90, p99, p999, messages, lost sql.NullInt64
		var lossPct sql.NullFloat64
		var sID, sRole, sAZ, sVPC, sSubnet, sPG, sPGS, sType sql.NullString
		var dID, dRole, dAZ, dVPC, dSubnet, dPG, dPGS, dType sql.NullString

		dest := []any{&kind, &variation, &srcIP, &dstIP, &unix, &p50, &p90, &p99, &p999,
			&messages, &lost, &lossPct}
		if meta {
			dest = append(dest,
				&sID, &sRole, &sAZ, &sVPC, &sSubnet, &sPG, &sPGS, &sType,
				&dID, &dRole, &dAZ, &dVPC, &dSubnet, &dPG, &dPGS, &dType)
		}
		if err := rows.Scan(dest...); err != nil {
			return ToolResult{SQL: q}, fmt.Errorf("scan: %w", err)
		}

		row := map[string]any{
			"kind":      kind,
			"variation": variation,
			"src_ip":    srcIP,
			"dst_ip":    dstIP,
			"unix":      unix,
			"p50":       p50,
			"p90":       nullInt(p90),
			"p99":       nullInt(p99),
			"p999":      nullInt(p999),
			"messages":  nullInt(messages),
			"lost":      nullInt(lost),
			"loss_pct":  nullFloat(lossPct),
		}
		if meta {
			row["src_instance_id"] = nullStr(sID)
			row["src_role"] = nullStr(sRole)
			row["src_az"] = nullStr(sAZ)
			row["src_vpc_id"] = nullStr(sVPC)
			row["src_subnet_id"] = nullStr(sSubnet)
			row["src_placement_group"] = nullStr(sPG)
			row["src_pg_strategy"] = nullStr(sPGS)
			row["src_instance_type"] = nullStr(sType)
			row["dst_instance_id"] = nullStr(dID)
			row["dst_role"] = nullStr(dRole)
			row["dst_az"] = nullStr(dAZ)
			row["dst_vpc_id"] = nullStr(dVPC)
			row["dst_subnet_id"] = nullStr(dSubnet)
			row["dst_placement_group"] = nullStr(dPG)
			row["dst_pg_strategy"] = nullStr(dPGS)
			row["dst_instance_type"] = nullStr(dType)
			// Derived, because "is this pair co-located" is the usual question.
			if sAZ.Valid && dAZ.Valid {
				row["same_az"] = sAZ.String == dAZ.String
			}
			if sVPC.Valid && dVPC.Valid {
				row["same_vpc"] = sVPC.String == dVPC.String
			}
			if sPG.Valid && dPG.Valid && sPG.String != "" {
				row["same_placement_group"] = sPG.String == dPG.String
			}
		}
		results = append(results, row)
	}
	if results == nil {
		results = []any{}
	}
	return ToolResult{Rows: results, SQL: q}, rows.Err()
}

// --- helpers ---

func nullInt(n sql.NullInt64) any {
	if n.Valid {
		return n.Int64
	}
	return nil
}

func nullFloat(n sql.NullFloat64) any {
	if n.Valid {
		return n.Float64
	}
	return nil
}
