package main

import (
	"encoding/json"
	"fmt"
	"log"
	"sync"
	"sync/atomic"
	"time"

	"afxdp-cp/proto"

	"github.com/nats-io/nats.go"
)

// Orchestrator dispatches commands to agents and correlates their results by
// CmdID. It also runs multi-node campaigns that encode the hard constraints
// learned operationally (serial senders for ucast; mode barriers; clock gates).
type Orchestrator struct {
	nc  *nats.Conn
	reg *Registry
	hub *Hub

	mu      sync.Mutex
	pending map[string]chan proto.CommandResult
	seq     uint64

	running int32 // 0/1 — only one campaign at a time
	lastMcastFwd string // replicator's last-applied mcast fwd mode (skip redundant set_mode)
	cancel  int32 // set to 1 to request the running campaign abort at the next boundary
}

// NewOrchestrator creates an orchestrator and subscribes to fleet.result.* so
// it can correlate results by CmdID regardless of which subscription delivered the command.
func NewOrchestrator(nc *nats.Conn, reg *Registry, hub *Hub) (*Orchestrator, error) {
	o := &Orchestrator{nc: nc, reg: reg, hub: hub, pending: map[string]chan proto.CommandResult{}}
	if _, err := nc.Subscribe(proto.SubjectResultWildcard, o.onResult); err != nil {
		return nil, err
	}
	return o, nil
}

// onResult routes an inbound CommandResult to the goroutine waiting on its CmdID.
func (o *Orchestrator) onResult(m *nats.Msg) {
	var r proto.CommandResult
	if err := json.Unmarshal(m.Data, &r); err != nil {
		return
	}
	o.mu.Lock()
	ch := o.pending[r.CmdID]
	o.mu.Unlock()
	if ch != nil {
		select {
		case ch <- r:
		default:
		}
	}
}

// nextCmdID returns a unique command identifier combining the nanosecond timestamp
// and a per-process sequence counter.
func (o *Orchestrator) nextCmdID() string {
	return fmt.Sprintf("%d-%d", time.Now().UnixNano(), atomic.AddUint64(&o.seq, 1))
}

// Cancel requests the currently running campaign to abort at the next safe
// boundary (round for ucast, mode for mcast). In-flight per-pair measurements
// already dispatched to agents run to completion; no new work is started.
func (o *Orchestrator) Cancel() { atomic.StoreInt32(&o.cancel, 1) }
func (o *Orchestrator) cancelled() bool { return atomic.LoadInt32(&o.cancel) == 1 }

// Dispatch publishes a command to a subject and waits for the agent's result.
func (o *Orchestrator) Dispatch(subject string, c proto.Command, timeout time.Duration) (proto.CommandResult, error) {
	c.CmdID = o.nextCmdID()
	ch := make(chan proto.CommandResult, 1)
	o.mu.Lock()
	o.pending[c.CmdID] = ch
	o.mu.Unlock()
	defer func() {
		o.mu.Lock()
		delete(o.pending, c.CmdID)
		o.mu.Unlock()
	}()
	b, err := json.Marshal(c)
	if err != nil {
		return proto.CommandResult{}, err
	}
	if err := o.nc.Publish(subject, b); err != nil {
		return proto.CommandResult{}, err
	}
	select {
	case r := <-ch:
		return r, nil
	case <-time.After(timeout):
		return proto.CommandResult{CmdID: c.CmdID, OK: false, Err: "timeout"}, fmt.Errorf("timeout waiting for %s", c.CmdID)
	}
}

// DispatchAgent is a convenience for the per-agent inbox.
func (o *Orchestrator) DispatchAgent(instanceID string, c proto.Command, timeout time.Duration) (proto.CommandResult, error) {
	return o.Dispatch(proto.SubjectCmdAgent(instanceID), c, timeout)
}

// dispatchRetry retries a command up to `attempts` times on error or !OK. Core
// NATS is at-most-once, so a dropped command/result is transient — a retry
// recovers the pair instead of leaving a hole in the matrix.
// dispatchRetry sends a command and retries on error or !OK up to attempts times.
// NATS is at-most-once delivery; a single retry recovers a dropped message.
func (o *Orchestrator) dispatchRetry(instanceID string, c proto.Command, timeout time.Duration, attempts int) (proto.CommandResult, error) {
	var res proto.CommandResult
	var err error
	for a := 0; a < attempts; a++ {
		res, err = o.DispatchAgent(instanceID, c, timeout)
		if err == nil && res.OK {
			return res, nil
		}
		if a < attempts-1 {
			time.Sleep(time.Second)
		}
	}
	return res, err
}

// scheduleRounds packs every ordered pair (i,j) of n nodes into concurrent rounds
// where no node index appears twice. All pairs within a round are node-disjoint and
// can run simultaneously without contending on a shared node. This turns the O(N²)
// serial matrix into ~2(N-1) rounds of up to N/2 concurrent pairs (O(N) wall-clock).
func scheduleRounds(n int) [][][2]int {
	var remaining [][2]int
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			if i != j {
				remaining = append(remaining, [2]int{i, j})
			}
		}
	}
	var rounds [][][2]int
	for len(remaining) > 0 {
		used := make([]bool, n)
		var round, next [][2]int
		for _, p := range remaining {
			if !used[p[0]] && !used[p[1]] {
				round = append(round, p)
				used[p[0]], used[p[1]] = true, true
			} else {
				next = append(next, p)
			}
		}
		rounds = append(rounds, round)
		remaining = next
	}
	return rounds
}

// UcastMatrixParams configures a serial NxN ucast campaign.
type UcastMatrixParams struct {
	Variation string `json:"variation"` // kernel|xdp-tx|xdp-rx|xdp-txrx
	Count     int    `json:"count"`
	Rate      int    `json:"rate"`
	Warmup    int    `json:"warmup"`
	DataPort  int    `json:"data_port"`
	ListenPort int   `json:"listen_port"`
}

// RunUcastMatrix runs the NxN serially — ONE sender at a time (the contention
// constraint), each measuring to every peer. Telemetry flows to the collector
// via the ingest path; here we sequence + await completion, emitting progress.
func (o *Orchestrator) RunUcastMatrix(p UcastMatrixParams) {
	if !atomic.CompareAndSwapInt32(&o.running, 0, 1) {
		o.hub.Emit("job", map[string]string{"status": "rejected", "reason": "a campaign is already running"})
		return
	}
	defer atomic.StoreInt32(&o.running, 0)
	atomic.StoreInt32(&o.cancel, 0)

	xdpTx := p.Variation == "xdp-tx" || p.Variation == "xdp-txrx"
	xdpRx := p.Variation == "xdp-rx" || p.Variation == "xdp-txrx"
	if p.Count == 0 {
		p.Count = 10000
	}
	if p.Rate == 0 {
		p.Rate = 10000
	}
	if p.Warmup == 0 {
		p.Warmup = 1000
	}
	if p.DataPort == 0 {
		p.DataPort = 5000
	}
	if p.ListenPort == 0 {
		p.ListenPort = 19020
	}
	nodes := o.reg.Online()
	if len(nodes) < 2 {
		o.hub.Emit("job", map[string]any{"status": "error", "kind": "ucast",
			"reason": fmt.Sprintf("need >=2 online nodes for an NxN matrix, have %d", len(nodes))})
		log.Printf("ucast campaign refused: only %d online node(s)", len(nodes))
		return
	}
	rounds := scheduleRounds(len(nodes))
	total := len(nodes) * (len(nodes) - 1)
	o.hub.Emit("job", map[string]any{"status": "running", "kind": "ucast", "variation": p.Variation,
		"pairs": total, "rounds": len(rounds)})
	log.Printf("campaign ucast/%s over %d nodes (%d pairs, %d parallel rounds)", p.Variation, len(nodes), total, len(rounds))

	// Prepare: only (re)set nodes that aren't already echoing in ucast mode — a
	// quick check against the last heartbeat state, so a heartbeat re-run is cheap
	// when the fleet is already prepared.
	var toPrep []Node
	for _, n := range nodes {
		if n.ReplicatorMode != "ucast" || n.ReplicatorSvc != "active" {
			toPrep = append(toPrep, n)
		}
	}
	if len(toPrep) > 0 {
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "ucast", "variation": p.Variation,
			"phase": "prepare", "msg": fmt.Sprintf("preparing %d/%d node(s) to ucast echo mode", len(toPrep), len(nodes))})
		var prep sync.WaitGroup
		for _, n := range toPrep {
			prep.Add(1)
			go func(n Node) {
				defer prep.Done()
				o.dispatchRetry(n.InstanceID, proto.Command{Type: proto.CmdSetMode, Mode: "ucast"}, 45*time.Second, 2)
			}(n)
		}
		prep.Wait()
	}
	o.mu.Lock()
	o.lastMcastFwd = "" // a ucast run leaves replicators in ucast mode
	o.mu.Unlock()

	var done int64
	perPair := 30 * time.Second
	for ri, round := range rounds {
		if o.cancelled() {
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "ucast", "variation": p.Variation,
				"done": atomic.LoadInt64(&done), "total": total})
			log.Printf("campaign ucast/%s cancelled after %d/%d", p.Variation, atomic.LoadInt64(&done), total)
			return
		}
		var wg sync.WaitGroup
		for _, pr := range round { // pairs in a round are node-disjoint -> safe to run concurrently
			s, d := nodes[pr[0]], nodes[pr[1]]
			wg.Add(1)
			// Pairs in this round are node-disjoint: no node is sender or receiver
			// in two simultaneous pairs, so they run without mutual interference.
			go func(s, d Node, ri int) {
				defer wg.Done()
				cmd := proto.Command{Type: proto.CmdRunRTT, RTT: &proto.RTTParams{
					TargetIP: d.PrivateIP, DataPort: p.DataPort, ListenIP: s.PrivateIP, ListenPort: p.ListenPort,
					Count: p.Count, Rate: p.Rate, Warmup: p.Warmup, SendCPU: -1, RecvCPU: -1,
					XdpTx: xdpTx, XdpTxQueue: 1, XdpRx: xdpRx,
				}}
				res, err := o.dispatchRetry(s.InstanceID, cmd, perPair, 2)
				n := atomic.AddInt64(&done, 1)
				ev := map[string]any{"status": "progress", "done": n, "total": total,
					"src": s.PrivateIP, "dst": d.PrivateIP, "round": ri + 1, "rounds": len(rounds)}
				if err != nil || !res.OK {
					ev["ok"] = false
					ev["err"] = firstErr(err, res.Err)
				} else {
					ev["ok"] = true
				}
				o.hub.Emit("job", ev)
			}(s, d, ri)
		}
		wg.Wait() // barrier: the next round starts only once this round's pairs finish
	}
	o.hub.Emit("job", map[string]any{"status": "done", "kind": "ucast", "variation": p.Variation,
		"done": atomic.LoadInt64(&done), "total": total})
	log.Printf("campaign ucast/%s complete (%d/%d)", p.Variation, atomic.LoadInt64(&done), total)
}

func firstErr(err error, s string) string {
	if err != nil {
		return err.Error()
	}
	return s
}

// McastMatrixParams configures a multicast fan-out campaign across fwd modes.
type McastMatrixParams struct {
	Modes      []string `json:"modes"` // subset of copy|inplace|kernel (default: all)
	Group      string   `json:"group"`
	DataPort   int      `json:"data_port"`
	Count      int      `json:"count"`
	IntervalUs int      `json:"interval_us"`
	TimeoutSec int      `json:"timeout_sec"`
}

// RunMcastMatrix drives the source -> replicator -> destination fan-out for each
// forward mode, encapsulating the proven setup: put the replicator in mcast
// mode+fwd, free the source/dest AF_XDP queues, (re)join destinations, gate on
// clock convergence, then run receivers concurrently with the source send.
func (o *Orchestrator) RunMcastMatrix(p McastMatrixParams) {
	if !atomic.CompareAndSwapInt32(&o.running, 0, 1) {
		o.hub.Emit("job", map[string]string{"status": "rejected", "reason": "a campaign is already running"})
		return
	}
	defer atomic.StoreInt32(&o.running, 0)
	atomic.StoreInt32(&o.cancel, 0)

	if len(p.Modes) == 0 {
		p.Modes = []string{"copy", "inplace", "kernel"}
	}
	if p.Group == "" {
		p.Group = "224.0.31.50"
	}
	if p.DataPort == 0 {
		p.DataPort = 5000
	}
	if p.Count == 0 {
		p.Count = 10000
	}
	if p.IntervalUs == 0 {
		p.IntervalUs = 100
	}
	if p.TimeoutSec == 0 {
		p.TimeoutSec = 60
	}

	source := o.reg.ByRole("source")
	replicator := o.reg.ByRole("replicator")
	dests := o.reg.AllByRole("destination")
	if source == nil || replicator == nil || len(dests) == 0 {
		o.hub.Emit("job", map[string]any{"status": "error",
			"reason": fmt.Sprintf("need source+replicator+destination roles online (have source=%v replicator=%v dests=%d)",
				source != nil, replicator != nil, len(dests))})
		return
	}
	o.hub.Emit("job", map[string]any{"status": "running", "kind": "mcast",
		"modes": p.Modes, "source": source.PrivateIP, "replicator": replicator.PrivateIP, "dests": len(dests)})
	log.Printf("campaign mcast: source=%s replicator=%s dests=%d modes=%v",
		source.PrivateIP, replicator.PrivateIP, len(dests), p.Modes)

	const svcT, runSetup = 45 * time.Second, 30 * time.Second

	// One-time: free the AF_XDP queue on the transient app nodes. Skip the (slow)
	// replicator STOP when it's already inactive from a prior run — but always run
	// the cheap cleanup (detach stale XDP / kill leftover procs) for robustness.
	if source.ReplicatorSvc == "active" {
		o.DispatchAgent(source.InstanceID, proto.Command{Type: proto.CmdReplicatorSvc, SvcAction: "stop"}, svcT)
	}
	o.DispatchAgent(source.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
	for _, d := range dests {
		if d.ReplicatorSvc == "active" {
			o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdReplicatorSvc, SvcAction: "stop"}, svcT)
		}
		o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
	}
	o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "phase": "prepare",
		"msg": "freed AF_XDP queues on source + destinations"})

	for _, mode := range p.Modes {
		// Replicator into mcast fan-out with this fwd mode.
		if o.cancelled() {
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes})
			log.Printf("campaign mcast cancelled")
			return
		}
		// Skip the (costly, replicator-restarting) set_mode when the replicator is
		// already in mcast/<mode> — a quick check for cheap heartbeat re-runs.
		o.mu.Lock()
		skip := replicator.ReplicatorMode == "mcast" && o.lastMcastFwd == mode
		o.mu.Unlock()
		if !skip {
			if res, err := o.DispatchAgent(replicator.InstanceID,
				proto.Command{Type: proto.CmdSetMode, Mode: "mcast", FwdMode: mode}, svcT); err != nil || !res.OK {
				o.hub.Emit("job", map[string]any{"status": "error", "mode": mode, "stage": "set_mode", "err": firstErr(err, res.Err)})
				continue
			}
			o.mu.Lock()
			o.lastMcastFwd = mode
			o.mu.Unlock()
		}
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
			"msg": "replicator in mcast/" + mode + " — destinations joining group + clock sync"})
		// Destinations (re)join the group behind the replicator + clock-gate.
		for _, d := range dests {
			o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdJoinGroup,
				Mcast: &proto.McastParams{ReplicatorIP: replicator.PrivateIP, Group: p.Group}}, runSetup)
			o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdClockSync}, runSetup)
		}
		o.DispatchAgent(source.InstanceID, proto.Command{Type: proto.CmdClockSync}, runSetup)
		if o.cancelled() {
			for _, d := range dests {
				o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
			}
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes})
			log.Printf("campaign mcast cancelled during %s setup", mode)
			return
		}
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
			"msg": fmt.Sprintf("sending %d packets source→replicator→%d dest(s)", p.Count, len(dests))})

		// Run (retryable): start each destination receiver (blocks in-agent until
		// count/timeout), fire the source send, await. On failure, retry the batch
		// once — setup already applied; a dropped NATS msg is transient.
		recvT := time.Duration(p.TimeoutSec+30) * time.Second
		type rr struct {
			dst string
			res proto.CommandResult
			err error
		}
		ok := false
		var results []rr
		for attempt := 1; attempt <= 2 && !ok; attempt++ {
			if o.cancelled() {
				break
			}
			var wg sync.WaitGroup
			results = make([]rr, len(dests))
			// Cancel watcher: if a cancel arrives mid-measurement, kill the in-flight
			// mcast_receive/mcast_send (cleanup) so the blocking dispatches return.
			stopWatch := make(chan struct{})
			go func() {
				t := time.NewTicker(500 * time.Millisecond)
				defer t.Stop()
				for {
					select {
					case <-stopWatch:
						return
					case <-t.C:
						if o.cancelled() {
							o.DispatchAgent(source.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
							for _, d := range dests {
								o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
							}
							return
						}
					}
				}
			}()
			for i, d := range dests {
				i, d := i, d
				wg.Add(1)
				go func() {
					defer wg.Done()
					res, err := o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdMcastReceive,
						Mcast: &proto.McastParams{Group: p.Group, DataPort: p.DataPort, Count: p.Count,
							TimeoutSec: p.TimeoutSec, ReplicatorIP: replicator.PrivateIP, SourceIP: source.PrivateIP,
							Variation: mode}}, recvT)
					results[i] = rr{d.PrivateIP, res, err}
				}()
			}
			time.Sleep(3 * time.Second) // receivers attach XDP before the send
			sres, serr := o.DispatchAgent(source.InstanceID, proto.Command{Type: proto.CmdMcastSend,
				Mcast: &proto.McastParams{Group: p.Group, DataPort: p.DataPort, ReplicatorIP: replicator.PrivateIP,
					Count: p.Count, IntervalUs: p.IntervalUs}}, recvT)
			wg.Wait()
			close(stopWatch)
			if o.cancelled() {
				ok = false
				break
			}
			ok = serr == nil && sres.OK
			for _, r := range results {
				if r.err != nil || !r.res.OK {
					ok = false
				}
			}
			if !ok && attempt < 2 {
				log.Printf("mcast/%s attempt %d failed; retrying", mode, attempt)
				for _, d := range dests {
					o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
				}
			}
		}
		if o.cancelled() {
			for _, d := range dests {
				o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
			}
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes})
			log.Printf("campaign mcast cancelled during %s run", mode)
			return
		}
		for _, r := range results {
			pairOK := r.err == nil && r.res.OK
			o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
				"src": source.PrivateIP, "dst": r.dst, "ok": pairOK, "err": firstErr(r.err, r.res.Err)})
		}
		for _, d := range dests { // release the queue for the next mode
			o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
		}
		o.hub.Emit("job", map[string]any{"status": "mode_done", "kind": "mcast", "mode": mode, "ok": ok})
		log.Printf("campaign mcast/%s done (ok=%v)", mode, ok)
	}
	o.hub.Emit("job", map[string]any{"status": "done", "kind": "mcast", "modes": p.Modes})
}
