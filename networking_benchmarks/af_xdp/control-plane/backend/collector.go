package main

import (
	"sync"

	"afxdp-cp/proto"
)

// Edge is one directed measurement (src -> dst) for a variation, latest value
// plus a short history ring for sparklines.
type Edge struct {
	Src       string        `json:"src"`
	Dst       string        `json:"dst"`
	Variation string        `json:"variation"`
	Kind      string        `json:"kind"`
	TxMode    string        `json:"tx_mode,omitempty"`
	Metrics   proto.Metrics `json:"metrics"`
	Unix      int64         `json:"unix"`
	History   []int64       `json:"history,omitempty"` // recent p50s
}

const edgeHistoryLen = 60

// Collector holds the authoritative in-memory NxN state.
type Collector struct {
	mu    sync.RWMutex
	edges map[string]*Edge // key: variation|src|dst
}

// NewCollector creates an empty NxN measurement matrix.
func NewCollector() *Collector { return &Collector{edges: map[string]*Edge{}} }

// edgeKey produces the map key for an edge. kind is included so mcast and ucast
// edges with the same variation name stay distinct (e.g. mcast/kernel vs ucast/kernel).
func edgeKey(kind, variation, src, dst string) string {
	return kind + "|" + variation + "|" + src + "|" + dst
}

// Apply records a telemetry sample and returns the updated edge (a copy).
func (c *Collector) Apply(t proto.Telemetry) Edge {
	c.mu.Lock()
	defer c.mu.Unlock()
	k := edgeKey(t.Kind, t.Variation, t.SrcIP, t.DstIP)
	e := c.edges[k]
	if e == nil {
		e = &Edge{Src: t.SrcIP, Dst: t.DstIP, Variation: t.Variation, Kind: t.Kind}
		c.edges[k] = e
	}
	e.Metrics = t.Metrics
	e.Unix = t.Unix
	if t.TxMode != "" {
		e.TxMode = t.TxMode
	}
	e.History = append(e.History, t.Metrics.ServiceRTT.P50)
	if len(e.History) > edgeHistoryLen {
		e.History = e.History[len(e.History)-edgeHistoryLen:]
	}
	return *e
}

// Snapshot returns a copy of all edges.
func (c *Collector) Snapshot() []Edge {
	c.mu.RLock()
	defer c.mu.RUnlock()
	out := make([]Edge, 0, len(c.edges))
	for _, e := range c.edges {
		out = append(out, *e)
	}
	return out
}
