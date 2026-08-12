package registry

import (
	"sync"
	"time"

	"afxdp-cp/proto"
)

// Node is the backend's live view of a fleet node: its self-reported topology
// plus liveness/state from heartbeats.
type Node struct {
	proto.NodeInfo
	AgentVersion   string  `json:"agent_version"`
	IsolCPUs       string  `json:"isolcpus,omitempty"`
	State          string  `json:"state"`
	ReplicatorMode string  `json:"replicator_mode,omitempty"`
	ReplicatorSvc  string  `json:"replicator_svc,omitempty"`
	ClockOffsetUs  float64 `json:"clock_offset_us"`
	LastSeenUnix   int64   `json:"last_seen_unix"`
	Online         bool    `json:"online"`
}

// Registry is the authoritative in-memory fleet, keyed by InstanceID.
type Registry struct {
	mu       sync.RWMutex
	nodes    map[string]*Node
	staleSec int64
}

// NewRegistry creates a fleet registry that marks nodes offline after staleSec
// seconds without a heartbeat.
func NewRegistry(staleSec int64) *Registry {
	return &Registry{nodes: map[string]*Node{}, staleSec: staleSec}
}

// Upsert applies a Registration (self-report on connect). Returns the node.
func (r *Registry) Upsert(reg proto.Registration) *Node {
	r.mu.Lock()
	defer r.mu.Unlock()
	n := r.nodes[reg.Node.InstanceID]
	if n == nil {
		n = &Node{}
		r.nodes[reg.Node.InstanceID] = n
	}
	n.NodeInfo = reg.Node
	n.AgentVersion = reg.AgentVersion
	n.IsolCPUs = reg.IsolCPUs
	n.LastSeenUnix = time.Now().Unix()
	n.Online = true
	if n.State == "" {
		n.State = "idle"
	}
	return n
}

// Heartbeat updates liveness and state for a known node. Returns the node and
// whether a material field (state, replicator mode, or online status) changed.
// Returns nil when the instance is not in the registry.
func (r *Registry) Heartbeat(hb proto.Heartbeat) (*Node, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	n := r.nodes[hb.InstanceID]
	if n == nil {
		return nil, false
	}
	prevState, prevMode, prevOnline := n.State, n.ReplicatorMode, n.Online
	n.State = hb.State
	n.ReplicatorMode = hb.ReplicatorMode
	n.ReplicatorSvc = hb.ReplicatorSvc
	n.ClockOffsetUs = hb.ClockOffsetUs
	n.LastSeenUnix = hb.Unix
	// Online is computed from the heartbeat's own Unix timestamp so List() and
	// Heartbeat() agree regardless of call order.
	n.Online = time.Now().Unix()-hb.Unix <= r.staleSec
	changed := prevState != n.State || prevMode != n.ReplicatorMode || prevOnline != n.Online
	return n, changed
}

// List returns a copy of all nodes, marking any past the stale window offline.
func (r *Registry) List() []Node {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := time.Now().Unix()
	out := make([]Node, 0, len(r.nodes))
	for _, n := range r.nodes {
		if now-n.LastSeenUnix > r.staleSec {
			n.Online = false
		}
		out = append(out, *n)
	}
	return out
}

// Replicators returns online nodes whose role is replicator (or unset — the
// ucast scenario leaves role default). Used to scope an NxN campaign.
func (r *Registry) Replicators() []Node {
	var out []Node
	for _, n := range r.List() {
		if n.Online && (n.Role == "replicator" || n.Role == "") {
			out = append(out, n)
		}
	}
	return out
}

// Online returns every node currently within the liveness window — the NxN
// ucast campaign runs over all of them regardless of role tag.
func (r *Registry) Online() []Node {
	var out []Node
	for _, n := range r.List() {
		if n.Online {
			out = append(out, n)
		}
	}
	return out
}

// ByRole returns the first online node with the given role, or nil.
func (r *Registry) ByRole(role string) *Node {
	for _, n := range r.List() {
		if n.Online && n.Role == role {
			nn := n
			return &nn
		}
	}
	return nil
}

// AllByRole returns all online nodes with the given role.
func (r *Registry) AllByRole(role string) []Node {
	var out []Node
	for _, n := range r.List() {
		if n.Online && n.Role == role {
			out = append(out, n)
		}
	}
	return out
}
