package hub

import (
	"encoding/json"
	"sync"
)

// Hub fans out live events to connected SSE clients. Slow clients are dropped
// rather than blocking the ingest path.
type Hub struct {
	mu      sync.Mutex
	clients map[chan []byte]struct{}
}

// NewHub creates an SSE fan-out hub. Slow clients are dropped rather than blocked.
func NewHub() *Hub { return &Hub{clients: map[chan []byte]struct{}{}} }

// Subscribe returns a new channel that receives serialised SSE payloads.
func (h *Hub) Subscribe() chan []byte {
	ch := make(chan []byte, 256)
	h.mu.Lock()
	h.clients[ch] = struct{}{}
	h.mu.Unlock()
	return ch
}

// Unsubscribe removes ch from the hub and closes it.
func (h *Hub) Unsubscribe(ch chan []byte) {
	h.mu.Lock()
	if _, ok := h.clients[ch]; ok {
		delete(h.clients, ch)
		close(ch)
	}
	h.mu.Unlock()
}

// Emit serialises {type, data} as JSON and broadcasts the payload to all subscribers.
// Subscribers that fall behind (channel full) are dropped without blocking.
func (h *Hub) Emit(typ string, data any) {
	b, err := json.Marshal(struct {
		Type string `json:"type"`
		Data any    `json:"data"`
	}{typ, data})
	if err != nil {
		return
	}
	h.mu.Lock()
	for ch := range h.clients {
		select {
		case ch <- b:
		default: // drop for a slow client
		}
	}
	h.mu.Unlock()
}
