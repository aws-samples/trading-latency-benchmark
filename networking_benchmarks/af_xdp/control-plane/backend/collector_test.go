package main

import (
	"sync"
	"testing"

	"afxdp-cp/proto"
)

func tel(variation, src, dst string, p50 int64) proto.Telemetry {
	return proto.Telemetry{Kind: "ucast", Variation: variation, SrcIP: src, DstIP: dst, Unix: 1,
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: p50}}}
}

func TestCollectorApplyAndKey(t *testing.T) {
	c := NewCollector()
	c.Apply(tel("kernel", "a", "b", 10))
	c.Apply(tel("kernel", "a", "b", 12)) // same edge -> update
	c.Apply(tel("xdp-tx", "a", "b", 5))  // different variation -> distinct edge
	c.Apply(tel("kernel", "b", "a", 9))  // reverse direction -> distinct edge
	if got := len(c.Snapshot()); got != 3 {
		t.Fatalf("want 3 distinct edges, got %d", got)
	}
	for _, e := range c.Snapshot() {
		if e.Variation == "kernel" && e.Src == "a" && e.Dst == "b" {
			if e.Metrics.ServiceRTT.P50 != 12 {
				t.Fatalf("edge should hold latest p50=12, got %d", e.Metrics.ServiceRTT.P50)
			}
			if len(e.History) != 2 {
				t.Fatalf("history should have 2 samples, got %d", len(e.History))
			}
		}
	}
}

func TestCollectorHistoryRing(t *testing.T) {
	c := NewCollector()
	for i := 0; i < edgeHistoryLen+25; i++ {
		c.Apply(tel("kernel", "a", "b", int64(i)))
	}
	e := c.Snapshot()[0]
	if len(e.History) != edgeHistoryLen {
		t.Fatalf("history ring should cap at %d, got %d", edgeHistoryLen, len(e.History))
	}
	if e.History[len(e.History)-1] != int64(edgeHistoryLen+24) {
		t.Fatalf("ring should keep most-recent sample, got %d", e.History[len(e.History)-1])
	}
}

// Regression: an mcast fwd mode named "kernel" must not clobber the ucast
// "kernel" variation edge for the same src->dst (they share a variation name).
func TestCollectorKindNamespacing(t *testing.T) {
	c := NewCollector()
	c.Apply(proto.Telemetry{Kind: "ucast", Variation: "kernel", SrcIP: "a", DstIP: "b",
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 30}}})
	c.Apply(proto.Telemetry{Kind: "mcast", Variation: "kernel", SrcIP: "a", DstIP: "b",
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 50}}})
	if got := len(c.Snapshot()); got != 2 {
		t.Fatalf("ucast+mcast kernel must be distinct edges, got %d", got)
	}
	for _, e := range c.Snapshot() {
		if e.Kind == "ucast" && e.Metrics.ServiceRTT.P50 != 30 {
			t.Fatalf("ucast kernel edge clobbered: p50=%d", e.Metrics.ServiceRTT.P50)
		}
		if e.Kind == "mcast" && e.Metrics.ServiceRTT.P50 != 50 {
			t.Fatalf("mcast kernel edge wrong: p50=%d", e.Metrics.ServiceRTT.P50)
		}
	}
}

// Concurrent writers (telemetry ingest) + readers (Snapshot for /api/fleet &
// new SSE clients) must not race or corrupt the NxN map. Run under -race.
func TestCollectorConcurrentLoad(t *testing.T) {
	c := NewCollector()
	stop := make(chan struct{})
	var readers, writers sync.WaitGroup

	for r := 0; r < 4; r++ {
		readers.Add(1)
		go func() {
			defer readers.Done()
			for {
				select {
				case <-stop:
					return
				default:
					_ = c.Snapshot()
				}
			}
		}()
	}
	vars := []string{"kernel", "xdp-tx", "copy"}
	for w := 0; w < 8; w++ {
		writers.Add(1)
		go func(w int) {
			defer writers.Done()
			for i := 0; i < 1500; i++ {
				c.Apply(tel(vars[i%len(vars)], "10.0.0."+itoa(w), "10.0.1."+itoa(i%20), int64(i)))
			}
		}(w)
	}
	writers.Wait()
	close(stop)
	readers.Wait()
	if len(c.Snapshot()) == 0 {
		t.Fatal("expected edges after concurrent load")
	}
}

func itoa(i int) string {
	if i == 0 {
		return "0"
	}
	s := ""
	for i > 0 {
		s = string(rune('0'+i%10)) + s
		i /= 10
	}
	return s
}
