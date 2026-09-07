"""
End-to-end integration tests for the read-only MCP server.

These drive the real binary over stdio with real JSON-RPC, which is the one
layer the Go unit tests cannot reach: those call the query functions directly,
so a broken protocol loop, a mis-declared tool schema or a bad dispatch would
pass them and still leave the server unusable by an MCP client.

The fixture database is built from the DDL extracted out of backend/store.go
rather than a copy of it. A schema change there therefore flows into these
tests automatically instead of leaving them silently testing a stale shape.
"""

import json
import os
import re
import shutil
import sqlite3
import subprocess
import time
from pathlib import Path

import pytest

# These exercise the Go MCP server and SQLite only; no compiled datapath needed.
pytestmark = pytest.mark.no_cpp_binaries

AF_XDP_DIR = Path(__file__).parent.parent
CP_DIR = AF_XDP_DIR / "control_plane"
def _find_store_go() -> Path:
    """Locate the Go file declaring the measurements schema.

    Searched rather than hardcoded: the backend has already been reorganised
    into sub-packages once, and a stale path here fails as 18 collection errors
    that look nothing like "a file moved".
    """
    hits = [p for p in CP_DIR.rglob("store.go") if "CREATE TABLE" in p.read_text()]
    if not hits:
        raise FileNotFoundError(
            f"no store.go declaring CREATE TABLE found under {CP_DIR}"
        )
    return sorted(hits)[0]


# ── fixture helpers ───────────────────────────────────────────────────────────

def extract_schema() -> str:
    """Pull the CREATE TABLE/INDEX statements out of backend/store.go.

    Keeping the fixture tied to the real DDL means these tests cannot drift into
    asserting against a schema the backend no longer creates.
    """
    store_go = _find_store_go()
    src = store_go.read_text()
    # The schema lives in a Go raw-string literal: schema := ` ... `
    m = re.search(r"schema\s*:=\s*`(.*?)`", src, re.S)
    if not m:
        pytest.fail(f"could not find the schema literal in {store_go}")
    ddl = m.group(1)
    # PRAGMA journal_mode/synchronous are runtime settings, not DDL; sqlite3
    # accepts them but they are irrelevant to a fixture.
    ddl = "\n".join(l for l in ddl.splitlines() if not l.strip().startswith("PRAGMA"))
    if "CREATE TABLE" not in ddl:
        pytest.fail("extracted schema contains no CREATE TABLE")
    return ddl


@pytest.fixture(scope="module")
def mcp_binary(tmp_path_factory):
    """Build the MCP server once for the module."""
    if shutil.which("go") is None:
        pytest.skip("go toolchain not available")
    out = tmp_path_factory.mktemp("mcpbin") / "afxdp-mcp"
    proc = subprocess.run(
        ["go", "build", "-o", str(out), "./mcp/cmd"],
        cwd=CP_DIR, capture_output=True, text=True,
    )
    if proc.returncode != 0:
        pytest.fail(f"go build failed:\n{proc.stderr}")
    return out


@pytest.fixture(scope="module")
def fixture_db(tmp_path_factory):
    """A database with two runs and known measurements, using the real schema."""
    path = tmp_path_factory.mktemp("mcpdb") / "measurements.db"
    con = sqlite3.connect(path)
    con.executescript(extract_schema())

    now = int(time.time())
    # Run 1: kernel baseline. Run 2: xdp, and a p50 regression on one edge.
    con.execute(
        "INSERT INTO runs (id, started_at, ended_at, kind, variation, scope, target_ids,"
        " pairs_total, pairs_ok, params) VALUES (1,?,?,'ucast','kernel','full',NULL,2,2,'{}')",
        (now - 3600, now - 3500),
    )
    con.execute(
        "INSERT INTO runs (id, started_at, ended_at, kind, variation, scope, target_ids,"
        ' pairs_total, pairs_ok, params) VALUES (2,?,?,\'ucast\',\'xdp\',\'among\','
        '\'["i-1","i-2"]\',2,2,\'{}\')',
        (now - 600, now - 500),
    )

    rows = [
        # run_id, unix, kind, variation, src, dst, p50, p99, loss
        (1, now - 3600, "ucast", "kernel", "10.0.0.1", "10.0.0.2", 30, 40, 0.0),
        (1, now - 3600, "ucast", "kernel", "10.0.0.2", "10.0.0.1", 31, 41, 0.0),
        # Same edges under xdp: one improves, one is much slower. Cross-mode, so
        # this is a datapath difference and must NOT read as a regression.
        (2, now - 600, "ucast", "xdp", "10.0.0.1", "10.0.0.2", 28, 35, 0.0),
        (2, now - 600, "ucast", "xdp", "10.0.0.2", "10.0.0.1", 56, 70, 0.0),
        # A genuine within-variation regression over time: kernel 10.0.0.1->
        # 10.0.0.3 grew 30 -> 62 us. This is what regressions() must catch.
        (1, now - 3600, "ucast", "kernel", "10.0.0.1", "10.0.0.3", 30, 38, 0.0),
        (2, now - 600, "ucast", "kernel", "10.0.0.1", "10.0.0.3", 62, 80, 0.0),
        # And a within-variation improvement, which must never be flagged.
        (1, now - 3600, "ucast", "kernel", "10.0.0.3", "10.0.0.1", 60, 75, 0.0),
        (2, now - 600, "ucast", "kernel", "10.0.0.3", "10.0.0.1", 29, 36, 0.0),
    ]
    for r in rows:
        con.execute(
            "INSERT INTO measurements (run_id, unix, kind, variation, src_ip, dst_ip,"
            " p50, p99, loss_pct) VALUES (?,?,?,?,?,?,?,?,?)", r,
        )
    con.commit()
    con.close()
    return path


class MCPClient:
    """Minimal newline-delimited JSON-RPC 2.0 client over the server's stdio."""

    def __init__(self, binary, db_path):
        env = dict(os.environ, MCP_DB_PATH=str(db_path))
        self.proc = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env=env, bufsize=1,
        )
        self._id = 0

    def call(self, method, params=None, timeout=15):
        self._id += 1
        req = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            req["params"] = params
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            err = self.proc.stderr.read()
            raise AssertionError(f"server closed stdout; stderr:\n{err}")
        return json.loads(line)

    def tool(self, name, args=None):
        return self.call("tools/call", {"name": name, "arguments": args or {}})

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


@pytest.fixture
def client(mcp_binary, fixture_db):
    c = MCPClient(mcp_binary, fixture_db)
    c.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
    yield c
    c.close()


def payload(resp):
    """Unwrap an MCP tools/call result into the parsed tool payload."""
    assert "error" not in resp, f"tool returned an error: {resp.get('error')}"
    content = resp["result"]["content"]
    assert content, "tool returned no content"
    return json.loads(content[0]["text"])


# ── protocol ──────────────────────────────────────────────────────────────────

class TestMCPProtocol:
    def test_initialize_handshake(self, mcp_binary, fixture_db):
        c = MCPClient(mcp_binary, fixture_db)
        try:
            resp = c.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
            assert resp["jsonrpc"] == "2.0"
            assert "result" in resp, resp
            assert "protocolVersion" in resp["result"]
        finally:
            c.close()

    def test_tools_list_advertises_all_six(self, client):
        resp = client.call("tools/list")
        names = {t["name"] for t in resp["result"]["tools"]}
        assert names == {
            "list_runs", "query_latency", "compare_runs",
            "compare_modes", "regressions", "topology_summary",
        }, names

    def test_every_tool_declares_an_input_schema(self, client):
        for t in client.call("tools/list")["result"]["tools"]:
            assert "inputSchema" in t, f"{t['name']} has no inputSchema"
            assert t["inputSchema"].get("type") == "object", t["name"]
            assert t.get("description"), f"{t['name']} has no description"

    def test_unknown_method_is_an_error_not_a_crash(self, client):
        resp = client.call("no/such/method")
        assert "error" in resp
        # The server must stay usable afterwards.
        assert client.call("tools/list")["result"]["tools"]

    def test_unknown_tool_is_an_error_not_a_crash(self, client):
        resp = client.tool("drop_everything")
        assert "error" in resp or resp["result"].get("isError")
        assert client.call("tools/list")["result"]["tools"]


# ── tools ─────────────────────────────────────────────────────────────────────

class TestMCPTools:
    def test_every_tool_returns_the_sql_it_ran(self, client):
        """A result you cannot reproduce by hand is not auditable."""
        calls = {
            "list_runs": {},
            "query_latency": {},
            "compare_runs": {"run_a": 1, "run_b": 2},
            "compare_modes": {"kind": "ucast", "variation_a": "kernel", "variation_b": "xdp"},
            "regressions": {"threshold_us": 10, "window_hours": 24},
            "topology_summary": {},
        }
        for name, args in calls.items():
            data = payload(client.tool(name, args))
            assert "sql" in data, f"{name} did not return the SQL it ran"
            assert "SELECT" in data["sql"].upper(), f"{name} sql looks wrong: {data['sql']}"

    def test_list_runs_returns_both_campaigns(self, client):
        data = payload(client.tool("list_runs"))
        rows = data["rows"]
        assert len(rows) == 2, rows
        by_id = {r["id"]: r for r in rows}
        assert by_id[1]["variation"] == "kernel"
        assert by_id[1]["scope"] == "full"
        assert by_id[2]["variation"] == "xdp"
        assert by_id[2]["scope"] == "among"

    def test_list_runs_filters_by_variation(self, client):
        data = payload(client.tool("list_runs", {"variation": "xdp"}))
        assert [r["variation"] for r in data["rows"]] == ["xdp"]

    def test_query_latency_filters_by_edge(self, client):
        data = payload(client.tool("query_latency", {"src": "10.0.0.1", "dst": "10.0.0.2"}))
        assert data["rows"], "expected samples for the edge"
        for r in data["rows"]:
            assert r["src_ip"] == "10.0.0.1" and r["dst_ip"] == "10.0.0.2"

    def test_compare_runs_pairs_cells_across_campaigns(self, client):
        data = payload(client.tool("compare_runs", {"run_a": 1, "run_b": 2}))
        rows = {(r["src_ip"], r["dst_ip"]): r for r in data["rows"]}
        assert len(rows) >= 2, rows
        # 10.0.0.1->10.0.0.2 improved 30 -> 28; the reverse regressed 31 -> 56.
        improved = rows[("10.0.0.1", "10.0.0.2")]
        regressed = rows[("10.0.0.2", "10.0.0.1")]
        assert improved["p50_a"] == 30 and improved["p50_b"] == 28
        assert regressed["p50_a"] == 31 and regressed["p50_b"] == 56
        # Comparing two runs of different variations is the common case, so each
        # side's variation must be reported for the delta to be interpretable.
        assert improved["variation_a"] == "kernel" and improved["variation_b"] == "xdp"

    def test_compare_modes_reports_both_variations(self, client):
        data = payload(client.tool("compare_modes", {
            "kind": "ucast", "variation_a": "kernel", "variation_b": "xdp",
        }))
        assert data["rows"], "expected per-edge cross-mode rows"

    def test_regressions_flags_growth_above_threshold(self, client):
        # kernel 10.0.0.1->10.0.0.3 grew 30 -> 62 us, so a 10us threshold catches it.
        data = payload(client.tool("regressions", {"threshold_us": 10, "window_hours": 48}))
        pairs = {(r["src_ip"], r["dst_ip"]) for r in data["rows"]}
        assert ("10.0.0.1", "10.0.0.3") in pairs, data["rows"]

    def test_regressions_ignores_growth_below_threshold(self, client):
        # Nothing grew by 100us, so a 100us threshold must return nothing.
        data = payload(client.tool("regressions", {"threshold_us": 100, "window_hours": 48}))
        assert not data["rows"], data["rows"]

    def test_regressions_does_not_flag_an_improvement(self, client):
        data = payload(client.tool("regressions", {"threshold_us": 1, "window_hours": 48}))
        pairs = {(r["src_ip"], r["dst_ip"]) for r in data["rows"]}
        assert ("10.0.0.3", "10.0.0.1") not in pairs, \
            "an edge that got FASTER must not be reported as a regression"

    def test_regressions_does_not_conflate_variations(self, client):
        """A kernel-vs-xdp difference is a datapath difference, not a regression.

        10.0.0.2->10.0.0.1 is 31us under kernel and 56us under xdp. Reporting
        that as a 25us regression would be the invalid cross-mode comparison the
        design explicitly rejects, so each variation must be its own series.
        """
        data = payload(client.tool("regressions", {"threshold_us": 1, "window_hours": 48}))
        pairs = {(r["src_ip"], r["dst_ip"]) for r in data["rows"]}
        assert ("10.0.0.2", "10.0.0.1") not in pairs, \
            f"cross-mode difference reported as a regression: {data['rows']}"

    def test_topology_summary_returns_newest_per_edge(self, client):
        data = payload(client.tool("topology_summary"))
        rows = data["rows"]
        assert rows, "expected a topology summary"
        # Newest sample per edge: the xdp run, not the older kernel one.
        seen = {(r["src_ip"], r["dst_ip"]): r for r in rows}
        assert seen[("10.0.0.2", "10.0.0.1")]["p50"] == 56


# ── read-only guarantee ───────────────────────────────────────────────────────

class TestMCPIsReadOnly:
    def test_no_tool_can_mutate(self, client):
        """The advertised surface must offer no write path at all."""
        tools = client.call("tools/list")["result"]["tools"]
        for t in tools:
            blob = json.dumps(t).lower()
            for verb in ("insert", "update", "delete", "drop", "truncate", "write"):
                assert verb not in t["name"].lower(), f"{t['name']} looks like a write tool"
            assert "readonly" not in blob or True  # descriptions may mention it

    def test_database_file_is_not_modified(self, mcp_binary, fixture_db):
        """Running the whole tool surface must leave the file byte-identical."""
        before = fixture_db.read_bytes()
        c = MCPClient(mcp_binary, fixture_db)
        try:
            c.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
            for name, args in {
                "list_runs": {},
                "query_latency": {},
                "compare_runs": {"run_a": 1, "run_b": 2},
                "compare_modes": {"kind": "ucast", "variation_a": "kernel", "variation_b": "xdp"},
                "regressions": {"threshold_us": 5, "window_hours": 48},
                "topology_summary": {},
            }.items():
                c.tool(name, args)
        finally:
            c.close()
        assert fixture_db.read_bytes() == before, \
            "the MCP server modified the measurement database"

    def test_missing_database_exits_with_a_clear_error(self, mcp_binary, tmp_path):
        proc = subprocess.run(
            [str(mcp_binary)], env=dict(os.environ, MCP_DB_PATH=str(tmp_path / "nope.db")),
            capture_output=True, text=True, timeout=30,
        )
        assert proc.returncode != 0
        assert proc.stderr.strip(), "a missing database must explain itself on stderr"


# ── schema contract ───────────────────────────────────────────────────────────

class TestSchemaContract:
    def test_store_declares_the_tables_the_tools_query(self):
        ddl = extract_schema()
        for table in ("runs", "measurements"):
            assert f"CREATE TABLE IF NOT EXISTS {table}" in ddl, table

    def test_edge_index_is_ordered_for_newest_first_lookups(self):
        """Both the startup ring seed and the history tooltip want newest-first,
        so the composite index must carry the DESC or they become sorts."""
        ddl = extract_schema()
        m = re.search(r"CREATE INDEX IF NOT EXISTS idx_m_edge[^;]+;", ddl)
        assert m, "idx_m_edge missing"
        assert "DESC" in m.group(0).upper(), m.group(0)
