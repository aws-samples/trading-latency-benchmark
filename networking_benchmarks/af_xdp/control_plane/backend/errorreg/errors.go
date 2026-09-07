package errorreg

import (
	"encoding/json"
	"sync"

	"afxdp-cp/backend/hub"
	"afxdp-cp/proto"

	"github.com/nats-io/nats.go"
)

const maxErrorsPerNode = 20

// ErrorRegistry stores a bounded ring of recent errors per node.
type ErrorRegistry struct {
	mu     sync.RWMutex
	errors map[string][]proto.ErrorEvent // instance_id -> ring (newest last)
	hub    *hub.Hub
}

func NewErrorRegistry(nc *nats.Conn, hub *hub.Hub) *ErrorRegistry {
	r := &ErrorRegistry{errors: make(map[string][]proto.ErrorEvent), hub: hub}
	nc.Subscribe(proto.SubjectError, r.onError)
	return r
}

func (r *ErrorRegistry) onError(m *nats.Msg) {
	var ev proto.ErrorEvent
	if err := json.Unmarshal(m.Data, &ev); err != nil || ev.InstanceID == "" {
		return
	}
	r.mu.Lock()
	ring := r.errors[ev.InstanceID]
	ring = append(ring, ev)
	if len(ring) > maxErrorsPerNode {
		ring = ring[len(ring)-maxErrorsPerNode:]
	}
	r.errors[ev.InstanceID] = ring
	r.mu.Unlock()
	// Broadcast to web UI via SSE.
	r.hub.Emit("error", map[string]any{"instance_id": ev.InstanceID, "error": ev})
}

// ErrorsFor returns the error ring for a node.
func (r *ErrorRegistry) ErrorsFor(id string) []proto.ErrorEvent {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.errors[id]
}

// AllErrors returns the full registry (for the error log download).
func (r *ErrorRegistry) AllErrors() map[string][]proto.ErrorEvent {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make(map[string][]proto.ErrorEvent, len(r.errors))
	for k, v := range r.errors {
		out[k] = v
	}
	return out
}

// NodesWithErrors returns instance IDs that have at least one error.
func (r *ErrorRegistry) NodesWithErrors() []string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	var ids []string
	for id, errs := range r.errors {
		if len(errs) > 0 {
			ids = append(ids, id)
		}
	}
	return ids
}
