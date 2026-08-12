package hub

import (
	"sync"
	"testing"
)

// Hammer the hub from many goroutines: subscribers coming and going while
// producers broadcast. Under -race this catches send-on-closed-channel and
// lock misuse — the exact hazards on the live telemetry fan-out path.
func TestHubConcurrentFanout(t *testing.T) {
	h := NewHub()
	var wg sync.WaitGroup

	// Producers (mimic ingest emitting node/edge/job under load).
	for p := 0; p < 8; p++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 2000; i++ {
				h.Emit("edge", map[string]int{"i": i})
			}
		}()
	}
	// Subscribers churning: subscribe, drain a bit, unsubscribe.
	for s := 0; s < 16; s++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for r := 0; r < 50; r++ {
				ch := h.Subscribe()
				for k := 0; k < 5; k++ {
					select {
					case <-ch:
					default:
					}
				}
				h.Unsubscribe(ch)
			}
		}()
	}
	wg.Wait()
	// Idempotent double-unsubscribe must not panic.
	ch := h.Subscribe()
	h.Unsubscribe(ch)
	h.Unsubscribe(ch)
}
