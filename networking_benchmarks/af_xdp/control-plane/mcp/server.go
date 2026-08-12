package mcp

import (
	"encoding/json"
	"fmt"
)

// JSONRPCRequest is a JSON-RPC 2.0 request.
type JSONRPCRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id,omitempty"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
}

// JSONRPCResponse is a JSON-RPC 2.0 response.
type JSONRPCResponse struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id,omitempty"`
	Result  any             `json:"result,omitempty"`
	Error   *JSONRPCError   `json:"error,omitempty"`
}

// JSONRPCError is a JSON-RPC 2.0 error object.
type JSONRPCError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

// Server is the MCP protocol server.
type Server struct {
	db *DB
}

// NewServer creates a new MCP server backed by the given database.
func NewServer(db *DB) *Server {
	return &Server{db: db}
}

// Handle dispatches a JSON-RPC request and returns the response.
func (s *Server) Handle(req JSONRPCRequest) JSONRPCResponse {
	switch req.Method {
	case "initialize":
		return s.handleInitialize(req)
	case "notifications/initialized":
		// Client acknowledgement - no response needed for notifications
		return JSONRPCResponse{JSONRPC: "2.0", ID: req.ID, Result: map[string]any{}}
	case "tools/list":
		return s.handleToolsList(req)
	case "tools/call":
		return s.handleToolsCall(req)
	default:
		return JSONRPCResponse{
			JSONRPC: "2.0",
			ID:      req.ID,
			Error:   &JSONRPCError{Code: -32601, Message: "method not found: " + req.Method},
		}
	}
}

func (s *Server) handleInitialize(req JSONRPCRequest) JSONRPCResponse {
	return JSONRPCResponse{
		JSONRPC: "2.0",
		ID:      req.ID,
		Result: map[string]any{
			"protocolVersion": "2024-11-05",
			"capabilities": map[string]any{
				"tools": map[string]any{},
			},
			"serverInfo": map[string]any{
				"name":    "afxdp-latency-mcp",
				"version": "1.0.0",
			},
		},
	}
}

func (s *Server) handleToolsList(req JSONRPCRequest) JSONRPCResponse {
	tools := []map[string]any{
		{
			"name":        "list_runs",
			"description": "List measurement campaign runs with optional filters",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"kind":      map[string]any{"type": "string", "description": "Filter by kind (ucast, mcast)"},
					"variation": map[string]any{"type": "string", "description": "Filter by variation (kernel, xdp)"},
					"since":     map[string]any{"type": "integer", "description": "Unix timestamp - only runs started at or after"},
					"limit":     map[string]any{"type": "integer", "description": "Max rows to return (default 100)"},
				},
			},
		},
		{
			"name":        "query_latency",
			"description": "Query per-pair latency measurement history",
			"inputSchema": map[string]any{
				"type": "object",
				"properties": map[string]any{
					"src":       map[string]any{"type": "string", "description": "Source IP filter"},
					"dst":       map[string]any{"type": "string", "description": "Destination IP filter"},
					"kind":      map[string]any{"type": "string", "description": "Filter by kind"},
					"variation": map[string]any{"type": "string", "description": "Filter by variation"},
					"since":     map[string]any{"type": "integer", "description": "Unix timestamp lower bound"},
					"limit":     map[string]any{"type": "integer", "description": "Max rows (default 100)"},
				},
			},
		},
		{
			"name":        "compare_runs",
			"description": "Compare per-cell latency delta between two campaign runs",
			"inputSchema": map[string]any{
				"type":     "object",
				"required": []string{"run_a", "run_b"},
				"properties": map[string]any{
					"run_a": map[string]any{"type": "integer", "description": "First run ID (baseline)"},
					"run_b": map[string]any{"type": "integer", "description": "Second run ID (comparison)"},
				},
			},
		},
		{
			"name":        "compare_modes",
			"description": "Compare two variations (e.g. kernel vs xdp) across the fleet using newest measurement per edge",
			"inputSchema": map[string]any{
				"type":     "object",
				"required": []string{"kind", "variation_a", "variation_b"},
				"properties": map[string]any{
					"kind":        map[string]any{"type": "string", "description": "Measurement kind (ucast, mcast)"},
					"variation_a": map[string]any{"type": "string", "description": "First variation (baseline)"},
					"variation_b": map[string]any{"type": "string", "description": "Second variation"},
					"since":       map[string]any{"type": "integer", "description": "Only consider measurements after this unix timestamp"},
				},
			},
		},
		{
			"name":        "regressions",
			"description": "Find pairs whose p50 latency grew by more than threshold within the time window",
			"inputSchema": map[string]any{
				"type":     "object",
				"required": []string{"threshold_us", "window_hours"},
				"properties": map[string]any{
					"threshold_us": map[string]any{"type": "integer", "description": "Minimum p50 increase in microseconds to flag"},
					"window_hours": map[string]any{"type": "integer", "description": "Look-back window in hours"},
					"kind":         map[string]any{"type": "string", "description": "Filter by kind"},
					"variation":    map[string]any{"type": "string", "description": "Filter by variation"},
				},
			},
		},
		{
			"name":        "topology_summary",
			"description": "Fleet topology with the newest measurement sample per edge",
			"inputSchema": map[string]any{
				"type":       "object",
				"properties": map[string]any{},
			},
		},
	}
	return JSONRPCResponse{
		JSONRPC: "2.0",
		ID:      req.ID,
		Result:  map[string]any{"tools": tools},
	}
}

func (s *Server) handleToolsCall(req JSONRPCRequest) JSONRPCResponse {
	var call struct {
		Name      string         `json:"name"`
		Arguments map[string]any `json:"arguments"`
	}
	if err := json.Unmarshal(req.Params, &call); err != nil {
		return JSONRPCResponse{
			JSONRPC: "2.0",
			ID:      req.ID,
			Error:   &JSONRPCError{Code: -32602, Message: "invalid params: " + err.Error()},
		}
	}

	var result ToolResult
	var err error

	switch call.Name {
	case "list_runs":
		p := ListRunsParams{
			Kind:      getString(call.Arguments, "kind"),
			Variation: getString(call.Arguments, "variation"),
			Since:     getInt64(call.Arguments, "since"),
			Limit:     int(getInt64(call.Arguments, "limit")),
		}
		result, err = s.db.ListRuns(p)

	case "query_latency":
		p := QueryLatencyParams{
			Src:       getString(call.Arguments, "src"),
			Dst:       getString(call.Arguments, "dst"),
			Kind:      getString(call.Arguments, "kind"),
			Variation: getString(call.Arguments, "variation"),
			Since:     getInt64(call.Arguments, "since"),
			Limit:     int(getInt64(call.Arguments, "limit")),
		}
		result, err = s.db.QueryLatency(p)

	case "compare_runs":
		runA := getInt64(call.Arguments, "run_a")
		runB := getInt64(call.Arguments, "run_b")
		if runA == 0 || runB == 0 {
			return JSONRPCResponse{
				JSONRPC: "2.0",
				ID:      req.ID,
				Error:   &JSONRPCError{Code: -32602, Message: "run_a and run_b are required"},
			}
		}
		result, err = s.db.CompareRuns(runA, runB)

	case "compare_modes":
		kind := getString(call.Arguments, "kind")
		varA := getString(call.Arguments, "variation_a")
		varB := getString(call.Arguments, "variation_b")
		if kind == "" || varA == "" || varB == "" {
			return JSONRPCResponse{
				JSONRPC: "2.0",
				ID:      req.ID,
				Error:   &JSONRPCError{Code: -32602, Message: "kind, variation_a, and variation_b are required"},
			}
		}
		since := getInt64(call.Arguments, "since")
		result, err = s.db.CompareModes(kind, varA, varB, since)

	case "regressions":
		threshold := getInt64(call.Arguments, "threshold_us")
		window := getInt64(call.Arguments, "window_hours")
		if threshold == 0 || window == 0 {
			return JSONRPCResponse{
				JSONRPC: "2.0",
				ID:      req.ID,
				Error:   &JSONRPCError{Code: -32602, Message: "threshold_us and window_hours are required"},
			}
		}
		p := RegressionsParams{
			ThresholdUs: threshold,
			WindowHours: window,
			Kind:        getString(call.Arguments, "kind"),
			Variation:   getString(call.Arguments, "variation"),
		}
		result, err = s.db.Regressions(p)

	case "topology_summary":
		result, err = s.db.TopologySummary()

	default:
		return JSONRPCResponse{
			JSONRPC: "2.0",
			ID:      req.ID,
			Error:   &JSONRPCError{Code: -32602, Message: "unknown tool: " + call.Name},
		}
	}

	if err != nil {
		return JSONRPCResponse{
			JSONRPC: "2.0",
			ID:      req.ID,
			Result: map[string]any{
				"content": []map[string]any{{
					"type": "text",
					"text": fmt.Sprintf(`{"error":%q,"sql":%q}`, err.Error(), result.SQL),
				}},
				"isError": true,
			},
		}
	}

	// Marshal the result as JSON text content per MCP spec.
	output := map[string]any{
		"rows": result.Rows,
		"sql":  result.SQL,
	}
	text, _ := json.Marshal(output)
	return JSONRPCResponse{
		JSONRPC: "2.0",
		ID:      req.ID,
		Result: map[string]any{
			"content": []map[string]any{{
				"type": "text",
				"text": string(text),
			}},
		},
	}
}

// --- argument helpers ---

func getString(m map[string]any, key string) string {
	if m == nil {
		return ""
	}
	v, ok := m[key]
	if !ok {
		return ""
	}
	s, ok := v.(string)
	if !ok {
		return ""
	}
	return s
}

func getInt64(m map[string]any, key string) int64 {
	if m == nil {
		return 0
	}
	v, ok := m[key]
	if !ok {
		return 0
	}
	switch n := v.(type) {
	case float64:
		return int64(n)
	case int64:
		return n
	case json.Number:
		i, _ := n.Int64()
		return i
	}
	return 0
}
