package mcp

import (
	"fmt"
	"sync"
	"testing"
)

// The analysis surface must be read-only BY CONSTRUCTION, not by a session
// setting that a caller can turn off or that a freshly-pooled connection never
// received. These tests attack both of those routes.

// PRAGMA query_only is per-connection state. database/sql opens connections
// lazily and pools them, so a pragma issued once via conn.Exec lands on
// whichever connection served that call -- any other connection the pool opens
// later is unrestricted unless the DSN itself is read-only.
func TestReadOnlySurvivesConnectionPool(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	// Force the pool to hand out several distinct connections concurrently, then
	// try to write on each.
	const n = 8
	var wg sync.WaitGroup
	errs := make([]error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			c, err := db.conn.Conn(t.Context())
			if err != nil {
				errs[i] = fmt.Errorf("acquire conn: %w", err)
				return
			}
			defer c.Close()
			_, errs[i] = c.ExecContext(t.Context(),
				"INSERT INTO runs (started_at, kind, variation) VALUES (99, 'evil', 'evil')")
		}(i)
	}
	wg.Wait()

	for i, err := range errs {
		if err == nil {
			t.Fatalf("connection %d accepted a write: the read-only guarantee does not hold across the pool", i)
		}
	}
}

// A caller must not be able to lift the restriction from inside a query.
func TestReadOnlyCannotBeDisabled(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	// Best effort: the pragma may be rejected outright, which is fine.
	_, _ = db.conn.Exec("PRAGMA query_only = OFF")

	if _, err := db.conn.Exec(
		"INSERT INTO runs (started_at, kind, variation) VALUES (98, 'evil', 'evil')"); err == nil {
		t.Fatal("a write succeeded after PRAGMA query_only=OFF: read-only is not enforced by construction")
	}
}

// Mutations of existing measurement rows must be impossible too, not just
// inserts -- silently rewriting history would be the worst failure mode here.
func TestReadOnlyRejectsUpdateAndDelete(t *testing.T) {
	path := createFixtureDB(t)
	seedFixtureData(t, path)
	db := openTestDB(t, path)

	for _, q := range []string{
		"UPDATE measurements SET p50 = 1",
		"DELETE FROM measurements",
		"DROP TABLE measurements",
		"CREATE TABLE sneaky (x INTEGER)",
	} {
		if _, err := db.conn.Exec(q); err == nil {
			t.Fatalf("%q was accepted on a read-only connection", q)
		}
	}
}
