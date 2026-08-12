package api

import (
	"encoding/json"
	"io"
	"net/http"
	"os"
	"time"

	"afxdp-cp/backend/collector"
	"afxdp-cp/backend/errorreg"
	"afxdp-cp/backend/hub"
	"afxdp-cp/backend/orchestrator"
	"afxdp-cp/backend/registry"
	"afxdp-cp/proto"
)

// Server holds the dependencies for the HTTP API handlers.
type Server struct {
	Reg    *registry.Registry
	Coll   *collector.Collector
	Hub    *hub.Hub
	Orch   *orchestrator.Orchestrator
	ErrReg *errorreg.ErrorRegistry
	Web    string // static web dir (may be empty)
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
