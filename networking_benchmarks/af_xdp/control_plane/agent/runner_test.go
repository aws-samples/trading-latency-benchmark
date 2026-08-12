package main

import (
	"encoding/json"
	"testing"
)

// The rtt tool writes this shape to /tmp/rtt_results.json; the parser must map
// it into proto.Metrics faithfully (this is the schema the whole matrix keys on).
func TestRttJSONParse(t *testing.T) {
	raw := `{
		"messages": 10000, "lost": 3, "loss_pct": 0.03, "clock_skew_samples": 0,
		"service_rtt_us": {"min": 18, "mean": 25, "p50": 24, "p90": 31, "p95": 34, "p99": 45, "p999": 88, "max": 210}
	}`
	var j rttJSON
	if err := json.Unmarshal([]byte(raw), &j); err != nil {
		t.Fatal(err)
	}
	m := toMetrics(j)
	if m.Messages != 10000 || m.Lost != 3 || m.LossPct != 0.03 {
		t.Fatalf("counters wrong: %+v", m)
	}
	if m.ServiceRTT.P50 != 24 || m.ServiceRTT.P99 != 45 || m.ServiceRTT.Max != 210 || m.ServiceRTT.Min != 18 {
		t.Fatalf("percentiles wrong: %+v", m.ServiceRTT)
	}
}

func TestDerivePinsDefault(t *testing.T) {
	// Off-EC2 (no isolcpus in /proc/cmdline) -> lo defaults to 1 => send=4, recv=3
	// (IRQ=lo, poll=lo+1, recv=lo+2, send=lo+3).
	send, recv := derivePins()
	if recv >= send {
		t.Fatalf("recv (%d) should be below send (%d)", recv, send)
	}
	if send-recv != 1 {
		t.Fatalf("send/recv should be adjacent isolated cores, got send=%d recv=%d", send, recv)
	}
}

func TestParseCPUList(t *testing.T) {
	got := parseCPUList("1-4,6")
	want := []int{1, 2, 3, 4, 6}
	if len(got) != len(want) {
		t.Fatalf("want %v, got %v", want, got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("want %v, got %v", want, got)
		}
	}
	if len(parseCPUList("")) != 0 {
		t.Fatal("empty string should parse to empty list")
	}
	if x := parseCPUList("2"); len(x) != 1 || x[0] != 2 {
		t.Fatalf("single cpu parse wrong: %v", x)
	}
}
