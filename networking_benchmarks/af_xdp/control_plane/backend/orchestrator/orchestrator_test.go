package orchestrator

import (
	"encoding/json"
	"sync/atomic"
	"testing"

	"afxdp-cp/backend/registry"
	"afxdp-cp/proto"

	"github.com/nats-io/nats.go"
)

// onResult routing is testable without a live NATS: build an orchestrator with
// just the pending map, register a waiter, and feed it a synthetic result msg.
func TestOrchestratorResultCorrelation(t *testing.T) {
	o := &Orchestrator{pending: map[string]chan proto.CommandResult{}}
	ch := make(chan proto.CommandResult, 1)
	o.pending["cmd-42"] = ch

	b, _ := json.Marshal(proto.CommandResult{CmdID: "cmd-42", InstanceID: "i-x", OK: true, Text: "pong"})
	o.onResult(&nats.Msg{Data: b})

	select {
	case r := <-ch:
		if r.CmdID != "cmd-42" || !r.OK {
			t.Fatalf("bad routed result: %+v", r)
		}
	default:
		t.Fatal("result was not routed to the waiting channel")
	}

	// A result for an unknown CmdID must not panic or block.
	unk, _ := json.Marshal(proto.CommandResult{CmdID: "nobody", OK: true})
	o.onResult(&nats.Msg{Data: unk})
}

func TestNextCmdIDUnique(t *testing.T) {
	o := &Orchestrator{pending: map[string]chan proto.CommandResult{}}
	seen := map[string]bool{}
	for i := 0; i < 1000; i++ {
		id := o.nextCmdID()
		if seen[id] {
			t.Fatalf("duplicate CmdID: %s", id)
		}
		seen[id] = true
	}
}

// Cancel must set the flag; a campaign resets it at start (mirrored here).
func TestOrchestratorCancel(t *testing.T) {
	o := &Orchestrator{pending: map[string]chan proto.CommandResult{}}
	if o.cancelled() {
		t.Fatal("a fresh orchestrator must not be cancelled")
	}
	o.Cancel()
	if !o.cancelled() {
		t.Fatal("Cancel() must set the cancelled flag")
	}
	atomic.StoreInt32(&o.cancel, 0) // Run* clears it at campaign start
	if o.cancelled() {
		t.Fatal("resetting cancel must clear the flag")
	}
}

// Only one campaign may run at a time — the CAS guard used by RunUcastMatrix /
// RunMcastMatrix rejects a concurrent start and re-acquires after release.
func TestOnlyOneCampaignAtATime(t *testing.T) {
	o := &Orchestrator{pending: map[string]chan proto.CommandResult{}}
	if !atomic.CompareAndSwapInt32(&o.running, 0, 1) {
		t.Fatal("first campaign should acquire the guard")
	}
	if atomic.CompareAndSwapInt32(&o.running, 0, 1) {
		t.Fatal("a second concurrent campaign must be rejected")
	}
	atomic.StoreInt32(&o.running, 0) // deferred release at campaign end
	if !atomic.CompareAndSwapInt32(&o.running, 0, 1) {
		t.Fatal("guard must re-acquire after release")
	}
}

// nicTuningViolations must flag any key present in a node's tuning that
// differs from the baked baseline, and must NOT flag a key the node simply
// didn't report (the older-agent best-effort path).
func TestNicTuningViolationsDetectsDrift(t *testing.T) {
	baseline := map[string]string{
		"napi_defer_hard_irqs": "2",
		"gro_flush_timeout":    "10000",
		"rx_usecs":             "0",
		"tx_usecs":             "0",
		"adaptive_rx":          "off",
	}
	v := nicTuningViolations("src", "10.0.0.1", baseline)
	if len(v) != 0 {
		t.Fatalf("baseline-matching tuning must produce no violations, got %v", v)
	}

	drifted := map[string]string{
		"napi_defer_hard_irqs": "2",
		"gro_flush_timeout":    "10000",
		"rx_usecs":             "0",
		"tx_usecs":             "0",
		"adaptive_rx":          "on", // drifted from baked baseline
	}
	v = nicTuningViolations("repl", "10.0.0.2", drifted)
	if len(v) != 1 {
		t.Fatalf("expected exactly 1 violation for drifted adaptive_rx, got %v", v)
	}

	// A key the agent never reported (older agent, or unsupported on this
	// NIC) must be silently skipped, not treated as a mismatch.
	partial := map[string]string{
		"napi_defer_hard_irqs": "2",
		// gro_flush_timeout, rx_usecs, tx_usecs, adaptive_rx all absent
	}
	v = nicTuningViolations("dst", "10.0.0.3", partial)
	if len(v) != 0 {
		t.Fatalf("absent keys must not be flagged as violations, got %v", v)
	}
}

// The gate must reject a run outright when any node reports a drifted value -
// this is the behavior that promotes NIC tuning from metadata to a gate.
func TestNicTuningViolationsRejectsMultiNodeDrift(t *testing.T) {
	srcDrift := nicTuningViolations("src", "10.0.0.1", map[string]string{"rx_usecs": "10"})
	replOK := nicTuningViolations("repl", "10.0.0.2", map[string]string{"rx_usecs": "0"})
	all := append(srcDrift, replOK...)
	if len(all) != 1 {
		t.Fatalf("expected exactly 1 violation across nodes, got %v", all)
	}
}

// wanderTargets must return exactly source + replicator + every destination,
// and must de-duplicate a degenerate topology (source appearing again as a
// destination) rather than dispatching wander_start twice to the same node -
// wander-sampler-lifecycle-design.md §3 step 2.
func TestWanderTargets(t *testing.T) {
	src := registry.Node{NodeInfo: proto.NodeInfo{InstanceID: "i-src", PrivateIP: "10.0.0.1"}}
	repl := registry.Node{NodeInfo: proto.NodeInfo{InstanceID: "i-repl", PrivateIP: "10.0.0.2"}}
	d1 := registry.Node{NodeInfo: proto.NodeInfo{InstanceID: "i-d1", PrivateIP: "10.0.0.3"}}
	d2 := registry.Node{NodeInfo: proto.NodeInfo{InstanceID: "i-d2", PrivateIP: "10.0.0.4"}}

	got := wanderTargets(src, repl, []registry.Node{d1, d2})
	if len(got) != 4 {
		t.Fatalf("want 4 distinct nodes, got %d: %+v", len(got), got)
	}
	ids := map[string]bool{}
	for _, n := range got {
		ids[n.InstanceID] = true
	}
	for _, want := range []string{"i-src", "i-repl", "i-d1", "i-d2"} {
		if !ids[want] {
			t.Fatalf("missing %s in wander targets: %+v", want, got)
		}
	}

	// Degenerate: source also appears as a destination (e.g. a
	// single-instance dev fleet). Must be de-duplicated, not double-dispatched.
	dup := wanderTargets(src, repl, []registry.Node{src, d1})
	seen := map[string]int{}
	for _, n := range dup {
		seen[n.InstanceID]++
	}
	if seen["i-src"] != 1 {
		t.Fatalf("source appearing in modeDests must be de-duplicated, got count=%d", seen["i-src"])
	}
	if len(dup) != 3 {
		t.Fatalf("want 3 distinct nodes after dedup (src, repl, d1), got %d: %+v", len(dup), dup)
	}
}

// wanderTargets with zero destinations (e.g. an empty fleet slice mid-setup)
// must still return source+replicator, not panic on a nil/empty modeDests.
func TestWanderTargetsNoDests(t *testing.T) {
	src := registry.Node{NodeInfo: proto.NodeInfo{InstanceID: "i-src"}}
	repl := registry.Node{NodeInfo: proto.NodeInfo{InstanceID: "i-repl"}}
	got := wanderTargets(src, repl, nil)
	if len(got) != 2 {
		t.Fatalf("want 2 (src, repl), got %d: %+v", len(got), got)
	}
}

// wanderRunSeconds must exceed the mode's configured TimeoutSec - the
// sampler has to span the settle phase PLUS the run phase, so its own
// lifetime bound must never be tighter than the traffic run it's meant to
// cover (wander-sampler-lifecycle-design.md §3 step 2/§2.2).
func TestWanderRunSeconds(t *testing.T) {
	if got := wanderRunSeconds(60); got <= 60 {
		t.Fatalf("wanderRunSeconds(60) = %d, must exceed the run's own TimeoutSec", got)
	}
	if got := wanderRunSeconds(0); got <= 0 {
		t.Fatalf("wanderRunSeconds(0) = %d, must still be positive", got)
	}
}
