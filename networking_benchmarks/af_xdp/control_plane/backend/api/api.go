package api

import (
	"encoding/json"
	"io"
	"net/http"
	"os"
	"strconv"
	"time"

	"afxdp-cp/backend/collector"
	"afxdp-cp/backend/errorreg"
	"afxdp-cp/backend/hub"
	"afxdp-cp/backend/orchestrator"
	"afxdp-cp/backend/registry"
	"afxdp-cp/backend/store"
	"afxdp-cp/proto"
)

// Server holds the dependencies for the HTTP API handlers.
type Server struct {
	Reg    *registry.Registry
	Coll   *collector.Collector
	Hub    *hub.Hub
	Orch   *orchestrator.Orchestrator
	ErrReg *errorreg.ErrorRegistry
	Store  *store.Store // nil when persistence is disabled; guards handleMcastReplicators
	Web    string       // static web dir (may be empty)
}

// Routes returns the HTTP mux with all API routes registered.
func (s *Server) Routes() *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/fleet", s.handleFleet)
	mux.HandleFunc("/api/events", s.handleEvents)
	mux.HandleFunc("/api/run", s.handleRun)
	mux.HandleFunc("/api/cancel", s.handleCancel)
	mux.HandleFunc("/api/cmd", s.handleCmd)
	mux.HandleFunc("/api/errors", s.handleErrors)
	mux.HandleFunc("/api/mcast-replicators", s.handleMcastReplicators)
	mux.HandleFunc("/api/measurements", s.handleMeasurements)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) { w.Write([]byte("ok")) })
	if s.Web != "" {
		mux.Handle("/", http.FileServer(http.Dir(s.Web)))
	}
	return mux
}

// GET /api/fleet — full snapshot (nodes + edges), for late joiners / batch view.
func (s *Server) handleFleet(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"generated_unix": time.Now().Unix(),
		"nodes":          s.Reg.List(),
		"edges":          s.Coll.Snapshot(),
	})
}

// GET /api/events — SSE stream: snapshot on connect, then node/edge/job deltas.
func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	fl, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	// Initial snapshot.
	snap, _ := json.Marshal(struct {
		Type string `json:"type"`
		Data any    `json:"data"`
	}{"snapshot", map[string]any{"nodes": s.Reg.List(), "edges": s.Coll.Snapshot()}})
	writeSSE(w, snap)
	fl.Flush()

	ch := s.Hub.Subscribe()
	defer s.Hub.Unsubscribe(ch)
	ka := time.NewTicker(20 * time.Second)
	defer ka.Stop()
	ctx := r.Context()
	for {
		select {
		case <-ctx.Done():
			return
		case b, ok := <-ch:
			if !ok {
				return
			}
			writeSSE(w, b)
			fl.Flush()
		case <-ka.C:
			w.Write([]byte(": keepalive\n\n"))
			fl.Flush()
		}
	}
}

// POST /api/run — start a campaign (async). Body has "kind":"ucast"|"mcast"
// plus the matching params (UcastMatrixParams / McastMatrixParams).
func (s *Server) handleRun(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	var kind struct {
		Kind string `json:"kind"`
	}
	_ = json.Unmarshal(body, &kind)
	switch kind.Kind {
	case "mcast":
		var p orchestrator.McastMatrixParams
		if err := json.Unmarshal(body, &p); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		go s.Orch.RunMcastMatrix(p)
		writeJSON(w, http.StatusAccepted, map[string]any{"status": "started", "kind": "mcast", "modes": p.Modes})
	default: // ucast
		var p orchestrator.UcastMatrixParams
		if err := json.Unmarshal(body, &p); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if p.Variation == "" {
			p.Variation = "kernel"
		}
		go s.Orch.RunUcastMatrix(p)
		writeJSON(w, http.StatusAccepted, map[string]any{"status": "started", "kind": "ucast", "variation": p.Variation})
	}
}

// POST /api/cancel — request the running campaign to abort at the next boundary.
func (s *Server) handleCancel(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	s.Orch.Cancel()
	writeJSON(w, http.StatusAccepted, map[string]any{"status": "cancelling"})
}

// POST /api/cmd — dispatch an ad-hoc command to one agent. Body:
// {"instance_id":"i-...","command":{...proto.Command...}}
func (s *Server) handleCmd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		InstanceID string        `json:"instance_id"`
		Command    proto.Command `json:"command"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	res, err := s.Orch.DispatchAgent(req.InstanceID, req.Command, 60*time.Second)
	if err != nil {
		writeJSON(w, http.StatusGatewayTimeout, map[string]any{"ok": false, "err": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, res)
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeSSE(w http.ResponseWriter, data []byte) {
	w.Write([]byte("data: "))
	w.Write(data)
	w.Write([]byte("\n\n"))
}

// GET /api/errors — all node errors (or ?node=<id> for one node).
func (s *Server) handleErrors(w http.ResponseWriter, r *http.Request) {
	nodeID := r.URL.Query().Get("node")
	if nodeID != "" {
		writeJSON(w, http.StatusOK, s.ErrReg.ErrorsFor(nodeID))
	} else {
		writeJSON(w, http.StatusOK, s.ErrReg.AllErrors())
	}
}

// GET /api/mcast-replicators — per-replicator mcast latency history for the
// report page. The live /api/fleet snapshot cannot show this: its edges are
// keyed only by (kind, variation, src, dst) and get overwritten as later
// replicators in the sweep measure the same destinations, so only the store
// (one `runs` row per replicator x mode) retains every replicator's numbers.
// This is report-only data; the live 2D/3D dashboard is unaffected.
//
// Optional query param: since_run_id (int) scopes to one campaign's runs
// instead of all history. limit (int, default 500) caps rows returned.
func (s *Server) handleMcastReplicators(w http.ResponseWriter, r *http.Request) {
	if s.Store == nil {
		writeJSON(w, http.StatusOK, map[string]any{"results": []any{}, "note": "persistence disabled"})
		return
	}
	sinceRunID := int64(0)
	if v := r.URL.Query().Get("since_run_id"); v != "" {
		if n, err := strconv.ParseInt(v, 10, 64); err == nil {
			sinceRunID = n
		}
	}
	limit := 0
	if v := r.URL.Query().Get("limit"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			limit = n
		}
	}
	results, err := s.Store.LatestMcastReplicatorResults(sinceRunID, limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"results": results})
}

// defaultMeasurementsWindow bounds an unscoped GET /api/measurements to the
// last 24h so a report load never forces a scan of the full retention window
// (default 7 days) — mirrors the discipline handleMcastReplicators already
// applies via since_run_id, using time instead since ucast runs have no
// natural "campaign" grouping the way a mcast replicator sweep does.
const defaultMeasurementsWindow = 24 * time.Hour

// GET /api/measurements — the general, store-backed report data source: the
// latest measurement per (kind, variation, src, dst, replicator) edge. This
// backs every table on the report page (Latest measurements, per-mode
// heatmaps, All measurements) — the live /api/fleet snapshot cannot show
// more than one value per (src,dst) pair, which silently collapses
// multi-replicator mcast results to whichever replicator measured most
// recently. See handleMcastReplicators and dev/roadmap/mcast-replicator-selection.md.
//
// Optional query params: kind ("ucast"|"mcast", default both), since_unix
// (default now-24h), limit (default 2000, after dedup).
func (s *Server) handleMeasurements(w http.ResponseWriter, r *http.Request) {
	if s.Store == nil {
		writeJSON(w, http.StatusOK, map[string]any{"results": []any{}, "note": "persistence disabled"})
		return
	}
	kind := r.URL.Query().Get("kind")
	sinceUnix := time.Now().Add(-defaultMeasurementsWindow).Unix()
	if v := r.URL.Query().Get("since_unix"); v != "" {
		if n, err := strconv.ParseInt(v, 10, 64); err == nil {
			sinceUnix = n
		}
	}
	limit := 0
	if v := r.URL.Query().Get("limit"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			limit = n
		}
	}
	results, err := s.Store.LatestMeasurements(kind, sinceUnix, limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"results": results, "since_unix": sinceUnix})
}

// WebDirDefault finds a built frontend directory if present.
func WebDirDefault() string {
	// Prefer a built frontend if present.
	for _, d := range []string{"web/dist", "../web/dist"} {
		if fi, err := os.Stat(d); err == nil && fi.IsDir() {
			return d
		}
	}
	return ""
}
