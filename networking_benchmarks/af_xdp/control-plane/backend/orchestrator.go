package main

import (
	"encoding/json"
	"fmt"
	"log"
	"strings"
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

// UcastMatrixParams configures a serial NxN ucast campaign.
type UcastMatrixParams struct {
	Variation   string `json:"variation"`   // kernel|xdp
	Count       int    `json:"count"`
	Rate        int    `json:"rate"`
	Warmup      int    `json:"warmup"`
	MaxParallel int    `json:"max_parallel"` // max concurrent pairs per round (0 = unlimited, 1 = serial)
	DataPort    int    `json:"data_port"`
	ListenPort  int    `json:"listen_port"`

	// MaxLossPct rejects any pair whose loss exceeds this percentage instead of
	// recording its (survivorship-biased) percentiles. Negative disables the
	// gate; 0 means "use the default". See DefaultMaxLossPct.
	MaxLossPct float64 `json:"max_loss_pct"`
}

// DefaultMaxLossPct is the loss ceiling applied when UcastMatrixParams.MaxLossPct
// is left at 0. A few percent tolerates ordinary straggler drops while still
// rejecting the pathological runs (30-95% loss) that make percentiles meaningless.
const DefaultMaxLossPct = 2.0

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

	xdpTx := p.Variation == "xdp"
	xdpRx := p.Variation == "xdp"
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
	// 0 => apply the default gate. Negative => caller explicitly disabled it,
	// which we normalise to 0 so the agent-side check is skipped.
	if p.MaxLossPct == 0 {
		p.MaxLossPct = DefaultMaxLossPct
	} else if p.MaxLossPct < 0 {
		p.MaxLossPct = 0
	}
	nodes := o.reg.Online()
	if len(nodes) < 2 {
		o.hub.Emit("job", map[string]any{"status": "error", "kind": "ucast",
			"reason": fmt.Sprintf("need >=2 online nodes for an NxN matrix, have %d", len(nodes))})
		log.Printf("ucast campaign refused: only %d online node(s)", len(nodes))
		return
	}
	total := len(nodes) * (len(nodes) - 1)
	o.hub.Emit("job", map[string]any{"status": "running", "kind": "ucast", "variation": p.Variation,
		"pairs": total, "sources": len(nodes)})
	log.Printf("campaign ucast/%s over %d nodes (%d pairs, source-grouped, max %d concurrent per source)",
		p.Variation, len(nodes), total, p.MaxParallel)

	// Prepare: converge every node to the ucast echo profile. EnsureHostState is
	// idempotent, so nodes already correct cost one cheap state read and no
	// restart — we can therefore dispatch to all of them unconditionally instead
	// of guessing from a possibly-stale heartbeat.
	o.hub.Emit("job", map[string]any{"status": "progress", "kind": "ucast", "variation": p.Variation,
		"phase": "prepare", "msg": fmt.Sprintf("converging %d node(s) to ucast echo profile", len(nodes))})
	var prep sync.WaitGroup
	for _, n := range nodes {
		prep.Add(1)
		go func(n Node) {
			defer prep.Done()
			o.dispatchRetry(n.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
				Host: &proto.HostStateParams{Profile: proto.HostEchoUcast}}, 60*time.Second, 2)
		}(n)
	}
	prep.Wait()
	// Purge stale ucast destinations on EVERY node before measuring.
	//
	// `rtt` deregisters itself on exit, but a killed/crashed rtt (timeout, cancel,
	// OOM) leaves an entry behind. In ucast mode the replicator echoes each packet
	// to EVERY registered destination, so a single stale entry doubles the per-packet
	// TX work on that node and shifts its whole p50 distribution into the ms range.
	// Purging here makes each campaign start from a clean registry.
	o.hub.Emit("job", map[string]any{"status": "progress", "kind": "ucast", "variation": p.Variation,
		"phase": "prepare", "msg": "initiating test"})
	var purge sync.WaitGroup
	for _, n := range nodes {
		purge.Add(1)
		go func(n Node) {
			defer purge.Done()
			o.dispatchRetry(n.InstanceID, proto.Command{Type: proto.CmdPurgeDests}, 30*time.Second, 2)
		}(n)
	}
	purge.Wait()
	o.mu.Lock()
	o.lastMcastFwd = "" // a ucast run leaves replicators in ucast mode
	o.mu.Unlock()

	var done int64
	var rejected int64 // pairs refused by the loss gate (ran fine, numbers unusable)
	perPair := 30 * time.Second
	// MaxParallel caps concurrent pairs per round. 0 or negative = unlimited (legacy).
	// For correctness, use 1 (serial) or low values (2-4) to avoid NIC/softirq contention.
	maxPar := p.MaxParallel
	if maxPar <= 0 {
		maxPar = len(nodes) // effectively unlimited (more than any round can have)
	}

	for si, s := range nodes {
		if o.cancelled() {
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "ucast", "variation": p.Variation,
				"done": atomic.LoadInt64(&done), "total": total})
			log.Printf("campaign ucast/%s cancelled after %d/%d", p.Variation, atomic.LoadInt64(&done), total)
			o.dispatchRetry(s.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
				Host: &proto.HostStateParams{Profile: proto.HostEchoUcast}}, 60*time.Second, 1)
			return
		}

		// This node becomes the measurer: stop its replicator so no AF_XDP
		// zero-copy socket owns the RX queue its echoes return on, and attach the
		// XDP program standalone so xdp mode still stamps. Grouping the matrix by
		// SOURCE keeps this to exactly TWO transitions per node for the whole
		// campaign; flipping per node-disjoint round would cost O(rounds x nodes)
		// systemctl operations instead.
		hres, herr := o.dispatchRetry(s.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
			Host: &proto.HostStateParams{Profile: proto.HostClient}}, 60*time.Second, 2)
		if herr != nil || !hres.OK {
			o.hub.Emit("job", map[string]any{"status": "progress", "kind": "ucast", "variation": p.Variation,
				"phase": "prepare", "ok": false, "src": s.PrivateIP,
				"err": "could not put node into client profile: " + firstErr(herr, hres.Err)})
			continue
		}

		var wg sync.WaitGroup
		sem := make(chan struct{}, maxPar)
		for _, d := range nodes {
			if d.InstanceID == s.InstanceID {
				continue
			}
			wg.Add(1)
			sem <- struct{}{} // block if maxPar measurements already in flight
			go func(s, d Node) {
				defer wg.Done()
				defer func() { <-sem }()
				cmd := proto.Command{Type: proto.CmdRunRTT, RTT: &proto.RTTParams{
					TargetIP: d.PrivateIP, DataPort: p.DataPort, ListenIP: s.PrivateIP, ListenPort: p.ListenPort,
					Count: p.Count, Rate: p.Rate, Warmup: p.Warmup, SendCPU: -1, RecvCPU: -1,
					XdpTx: xdpTx, XdpTxQueue: 1, XdpRx: xdpRx, MaxLossPct: p.MaxLossPct,
				}}
				res, err := o.dispatchRetry(s.InstanceID, cmd, perPair, 2)
				n := atomic.AddInt64(&done, 1)
				ev := map[string]any{"status": "progress", "done": n, "total": total,
					"src": s.PrivateIP, "dst": d.PrivateIP, "source": si + 1, "sources": len(nodes)}
				if err != nil || !res.OK {
					ev["ok"] = false
					e := firstErr(err, res.Err)
					ev["err"] = e
					// Surface a loss-gate rejection distinctly from a genuine
					// failure: the run executed fine, we are refusing its numbers.
					if strings.Contains(e, "loss gate:") {
						ev["rejected"] = "loss"
						atomic.AddInt64(&rejected, 1)
					}
				} else {
					ev["ok"] = true
				}
				o.hub.Emit("job", ev)
			}(s, d)
		}
		wg.Wait() // all of this source's measurements are done

		// Restore this node to the echo profile so it can serve as a destination
		// for the remaining sources. Second and final transition for this node.
		o.dispatchRetry(s.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
			Host: &proto.HostStateParams{Profile: proto.HostEchoUcast}}, 60*time.Second, 2)
	}
	rej := atomic.LoadInt64(&rejected)
	o.hub.Emit("job", map[string]any{"status": "done", "kind": "ucast", "variation": p.Variation,
		"done": atomic.LoadInt64(&done), "total": total,
		"rejected_loss": rej, "max_loss_pct": p.MaxLossPct})
	if rej > 0 {
		log.Printf("campaign ucast/%s complete (%d/%d) — %d pair(s) REJECTED by the loss gate (>%.2f%% loss); "+
			"their percentiles were discarded, not recorded",
			p.Variation, atomic.LoadInt64(&done), total, rej, p.MaxLossPct)
	} else {
		log.Printf("campaign ucast/%s complete (%d/%d)", p.Variation, atomic.LoadInt64(&done), total)
	}
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
	// Converge source + destinations to the mcast endpoint profile: replicator
	// stopped and XDP detached, because mcast_send / mcast_receive bind their own
	// AF_XDP sockets and load their own program. EnsureHostState is idempotent, so
	// nodes already in that state cost one state read and no service work — this
	// replaces the previous heartbeat-guessing plus unconditional cleanup, and is
	// what makes a ucast -> mcast switchover converge in a single pass per node.
	var mprep sync.WaitGroup
	endpoints := append([]Node{*source}, dests...)
	for _, n := range endpoints {
		mprep.Add(1)
		go func(n Node) {
			defer mprep.Done()
			o.DispatchAgent(n.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
				Host: &proto.HostStateParams{Profile: proto.HostMcastEndpoint}}, runSetup)
		}(n)
	}
	mprep.Wait()
	o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "phase": "prepare",
		"msg": "freed AF_XDP queues on source + destinations"})

	for _, mode := range p.Modes {
		// Replicator into mcast fan-out with this fwd mode.
		if o.cancelled() {
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes})
			log.Printf("campaign mcast cancelled")
			return
		}
		// EnsureHostState is idempotent and collapses a mode+fwd change into a
		// single restart, so the cached-mode guard is only an extra fast path: if
		// the replicator is already in mcast/<mode> the agent does no service work.
		o.mu.Lock()
		skip := replicator.ReplicatorMode == "mcast" && o.lastMcastFwd == mode
		o.mu.Unlock()
		if !skip {
			if res, err := o.DispatchAgent(replicator.InstanceID,
				proto.Command{Type: proto.CmdEnsureHost, Host: &proto.HostStateParams{
					Profile: proto.HostMcastReplicator, FwdMode: mode}}, svcT); err != nil || !res.OK {
				o.hub.Emit("job", map[string]any{"status": "error", "mode": mode, "stage": "set_mode", "err": firstErr(err, res.Err)})
				continue
			}
			o.mu.Lock()
			o.lastMcastFwd = mode
			o.mu.Unlock()
		}
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
			"msg": "replicator in mcast/" + mode + " — destinations joining group + clock sync"})
		// kernel (XDP_TX) mode is a single-destination passthrough — it cannot
		// fan out to multiple receivers. Use only the first destination as the
		// representative measurement; copy/inplace test the full fan-out.
		modeDests := dests
		if mode == "kernel" && len(dests) > 1 {
			modeDests = dests[:1]
			o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
				"msg": fmt.Sprintf("kernel mode: single-destination only (XDP_TX passthrough) — using %s", dests[0].PrivateIP)})
			log.Printf("mcast/kernel: limiting to 1 destination (%s) — XDP_TX is single-dest passthrough", dests[0].PrivateIP)
		}
		// Destinations (re)join the group behind the replicator + clock-gate.
		for _, d := range modeDests {
			o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdJoinGroup,
				Mcast: &proto.McastParams{ReplicatorIP: replicator.PrivateIP, Group: p.Group}}, runSetup)
			o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdClockSync}, runSetup)
		}
		o.DispatchAgent(source.InstanceID, proto.Command{Type: proto.CmdClockSync}, runSetup)
		if o.cancelled() {
			for _, d := range modeDests {
				o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
			}
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes})
			log.Printf("campaign mcast cancelled during %s setup", mode)
			return
		}
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
			"msg": fmt.Sprintf("sending %d packets source→replicator→%d dest(s)", p.Count, len(modeDests))})

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
			results = make([]rr, len(modeDests))
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
							for _, d := range modeDests {
								o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
							}
							return
						}
					}
				}
			}()
			for i, d := range modeDests {
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
				for _, d := range modeDests {
					o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
				}
			}
		}
		if o.cancelled() {
			for _, d := range modeDests {
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
		for _, d := range modeDests { // release the queue for the next mode
			o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
		}
		o.hub.Emit("job", map[string]any{"status": "mode_done", "kind": "mcast", "mode": mode, "ok": ok})
		log.Printf("campaign mcast/%s done (ok=%v)", mode, ok)
	}
	o.hub.Emit("job", map[string]any{"status": "done", "kind": "mcast", "modes": p.Modes})
}
