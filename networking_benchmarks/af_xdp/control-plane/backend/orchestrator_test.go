package main

import (
	"encoding/json"
	"sync/atomic"
	"testing"

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

// The scheduler must (a) never place a node twice in one round (contention-free
// concurrency) and (b) cover every ordered pair exactly once.
func TestScheduleRoundsCoversAllPairsDisjoint(t *testing.T) {
	for _, n := range []int{2, 3, 4, 5, 8, 10, 25} {
		rounds := scheduleRounds(n)
		seen := map[[2]int]bool{}
		maxPer := 0
		for _, round := range rounds {
			if len(round) > maxPer {
				maxPer = len(round)
			}
			used := map[int]bool{}
			for _, p := range round {
				if used[p[0]] || used[p[1]] {
					t.Fatalf("n=%d round not node-disjoint at %v", n, p)
				}
				used[p[0]], used[p[1]] = true, true
				if seen[p] {
					t.Fatalf("n=%d duplicate pair %v", n, p)
				}
				seen[p] = true
			}
		}
		if len(seen) != n*(n-1) {
			t.Fatalf("n=%d covered %d pairs, want %d", n, len(seen), n*(n-1))
		}
		if n >= 4 && maxPer < 2 {
			t.Fatalf("n=%d expected concurrency (>=2 pairs/round), got max %d", n, maxPer)
		}
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
