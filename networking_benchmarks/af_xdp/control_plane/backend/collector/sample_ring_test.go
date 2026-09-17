package collector

import (
	"testing"

	"afxdp-cp/proto"
)

// Tests for the richer Sample ring (Phase 3.1): History is []Sample, each
// carrying Unix, P50, P99. The ring caps at EdgeHistoryLen=60, newest last.

func TestSampleRingBasic(t *testing.T) {
	c := NewCollector()
	c.Apply(proto.Telemetry{
		Kind: "ucast", Variation: "kernel", SrcIP: "a", DstIP: "b", Unix: 100,
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 30, P99: 40}},
	})
	c.Apply(proto.Telemetry{
		Kind: "ucast", Variation: "kernel", SrcIP: "a", DstIP: "b", Unix: 200,
		Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: 35, P99: 45}},
	})
	e := c.Snapshot()[0]
	if len(e.History) != 2 {
		t.Fatalf("want 2 samples, got %d", len(e.History))
	}
	// Newest must be last (chronological order).
	if e.History[0].Unix != 100 || e.History[1].Unix != 200 {
		t.Fatalf("samples not in chronological order: %+v", e.History)
	}
	if e.History[1].P50 != 35 || e.History[1].P99 != 45 {
		t.Fatal("newest sample must carry P50 and P99")
	}
}

func TestSampleRingCapAt60(t *testing.T) {
	c := NewCollector()
	for i := 0; i < EdgeHistoryLen+20; i++ {
		c.Apply(proto.Telemetry{
			Kind: "ucast", Variation: "xdp", SrcIP: "x", DstIP: "y", Unix: int64(i),
			Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: int64(i * 10), P99: int64(i * 20)}},
		})
	}
	e := c.Snapshot()[0]
	if len(e.History) != EdgeHistoryLen {
		t.Fatalf("ring must cap at %d, got %d", EdgeHistoryLen, len(e.History))
	}
	// The newest sample (index 79) must be last.
	last := e.History[len(e.History)-1]
	wantUnix := int64(EdgeHistoryLen + 19)
	if last.Unix != wantUnix {
		t.Fatalf("newest sample unix: want %d, got %d", wantUnix, last.Unix)
	}
	// The oldest retained is index 20 (dropped 0..19).
	first := e.History[0]
	if first.Unix != 20 {
		t.Fatalf("oldest retained unix: want 20, got %d", first.Unix)
	}
}

func TestSampleRingNewestLast(t *testing.T) {
	c := NewCollector()
	times := []int64{50, 51, 52, 53, 54}
	for _, ts := range times {
		c.Apply(proto.Telemetry{
			Kind: "ucast", Variation: "kernel", SrcIP: "p", DstIP: "q", Unix: ts,
			Metrics: proto.Metrics{ServiceRTT: proto.Pct{P50: ts, P99: ts + 5}},
		})
	}
	e := c.Snapshot()[0]
	for idx := 1; idx < len(e.History); idx++ {
		if e.History[idx].Unix <= e.History[idx-1].Unix {
			t.Fatalf("history must be monotonically ordered (newest last), violation at [%d]", idx)
		}
	}
}
