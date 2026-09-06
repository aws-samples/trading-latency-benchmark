package orchestrator

import (
	"encoding/json"
	"fmt"
	"log"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"afxdp-cp/backend/hub"
	"afxdp-cp/backend/pairs"
	"afxdp-cp/backend/registry"
	"afxdp-cp/backend/store"
	"afxdp-cp/proto"

	"github.com/nats-io/nats.go"
)

// Orchestrator dispatches commands to agents and correlates their results by
// CmdID. It also runs multi-node campaigns that encode the hard constraints
// learned operationally (serial senders for ucast; mode barriers; clock gates).
type Orchestrator struct {
	nc    *nats.Conn
	reg   *registry.Registry
	hub   *hub.Hub
	store *store.Store // durable measurement history; nil when persistence is disabled

	mu      sync.Mutex
	pending map[string]chan proto.CommandResult
	seq     uint64

	running      int32  // 0/1 — only one campaign at a time
	lastMcastFwd string // replicator's last-applied mcast fwd mode (skip redundant set_mode)
	cancel       int32  // set to 1 to request the running campaign abort at the next boundary
}

// NewOrchestrator creates an orchestrator and subscribes to fleet.result.* so
// it can correlate results by CmdID regardless of which subscription delivered the command.
func NewOrchestrator(nc *nats.Conn, reg *registry.Registry, hub *hub.Hub, store *store.Store) (*Orchestrator, error) {
	o := &Orchestrator{nc: nc, reg: reg, hub: hub, store: store,
		pending: map[string]chan proto.CommandResult{}}
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
func (o *Orchestrator) Cancel()         { atomic.StoreInt32(&o.cancel, 1) }
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
	Variation   string `json:"variation"` // kernel|xdp
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

	// Nodes optionally restricts the campaign to these instance IDs. Empty means
	// every online node, i.e. the full NxN mesh.
	Nodes []string `json:"nodes,omitempty"`
	// Scope expands Nodes into ordered pairs: among (default) | fanout | fanin.
	Scope string `json:"scope,omitempty"`

	// XdpTx/XdpRx independently override the client TX/RX transport, matching
	// what deploy/ansible/run_ucast.yaml already allows (separate xdp_tx/xdp_rx
	// booleans). nil => derive both from Variation=="xdp" (unchanged default
	// behavior: "kernel" means neither, "xdp" means both).
	XdpTx *bool `json:"xdp_tx,omitempty"`
	XdpRx *bool `json:"xdp_rx,omitempty"`
	// XdpTxQueue overrides the AF_XDP TX queue used when XdpTx is enabled
	// (mirrors rtt's `--xdp-tx=<queue>`). 0 => default (queue 1).
	XdpTxQueue int `json:"xdp_tx_queue,omitempty"`

	// SendCPU/RecvCPU pin the rtt client's TX/RX threads to specific cores
	// (mirrors rtt's positional send_cpu/recv_cpu args). CPU 0 is always the
	// OS/SSH housekeeping core in this fleet's isolcpus layout, so — like
	// every other 0-means-default field in this struct — 0 means "derive from
	// the isolated CPU set" (matches ansible's auto_pin default) rather than
	// "pin to CPU 0".
	SendCPU int `json:"send_cpu,omitempty"`
	RecvCPU int `json:"recv_cpu,omitempty"`
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
	// XdpTx/XdpRx, when explicitly set, override the Variation-derived default
	// so a caller can enable one leg without the other — the same independence
	// deploy/ansible/run_ucast.yaml already offers via separate xdp_tx/xdp_rx vars.
	if p.XdpTx != nil {
		xdpTx = *p.XdpTx
	}
	if p.XdpRx != nil {
		xdpRx = *p.XdpRx
	}
	xdpTxQueue := 1
	if p.XdpTxQueue != 0 {
		xdpTxQueue = p.XdpTxQueue
	}
	sendCPU, recvCPU := -1, -1
	if p.SendCPU != 0 {
		sendCPU = p.SendCPU
	}
	if p.RecvCPU != 0 {
		recvCPU = p.RecvCPU
	}
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
	online := o.reg.Online()
	// A target set scopes the campaign to a subset of pairs; empty = full mesh.
	nodes, destsFor, skipped, rerr := pairs.ResolvePairs(online, p.Nodes, p.Scope)
	if rerr != nil {
		o.hub.Emit("job", map[string]any{"status": "error", "kind": "ucast",
			"variation": p.Variation, "reason": rerr.Error()})
		log.Printf("ucast campaign refused: %v", rerr)
		return
	}
	// Converge the union of sources and destinations, not just the sources: a
	// destination still in client profile from an earlier run would not echo.
	prep := pairs.PrepareSet(nodes, destsFor)
	total := 0
	for _, s := range nodes {
		total += len(destsFor[s.InstanceID])
	}
	desc := pairs.ScopeDescription(p.Scope, len(p.Nodes)-len(skipped))
	ev := map[string]any{"status": "running", "kind": "ucast", "variation": p.Variation,
		"pairs": total, "sources": len(nodes), "scope": desc}
	if len(skipped) > 0 {
		// Proceed, but say which selected nodes are not being measured.
		sort.Strings(skipped)
		ev["skipped"] = skipped
		ev["msg"] = fmt.Sprintf("skipping %d selected node(s) that are not online: %s",
			len(skipped), strings.Join(skipped, ", "))
		log.Printf("ucast/%s: skipping offline/unknown selected nodes: %v", p.Variation, skipped)
	}
	o.hub.Emit("job", ev)

	// Anchor the campaign in the runs table so its measurements are attributable
	// to it later. Telemetry arrives on the ingest goroutine, which reads the
	// current run id from the store.
	targetJSON := ""
	if len(p.Nodes) > 0 {
		if b, err := json.Marshal(p.Nodes); err == nil {
			targetJSON = string(b)
		}
	}
	runID, rErr := o.store.InsertRun("ucast", p.Variation, pairs.ScopeName(p.Scope, len(p.Nodes)), targetJSON, total,
		map[string]any{"count": p.Count, "rate": p.Rate, "warmup": p.Warmup, "max_loss_pct": p.MaxLossPct})
	if rErr != nil {
		log.Printf("store: could not open run row: %v", rErr)
	}
	o.store.SetCurrentRun(runID)
	defer o.store.SetCurrentRun(0)

	log.Printf("campaign ucast/%s — %s: %d pairs over %d source(s), %d node(s) to converge, max %d concurrent per source",
		p.Variation, desc, total, len(nodes), len(prep), p.MaxParallel)

	// Phase timers. Each accumulates wall-clock across the campaign so the `done`
	// event carries a breakdown of where the run actually spent its time.
	campaignStart := time.Now()
	var msPrepare, msClientTransition, msMeasure, msRestore int64
	// Restores run in goroutines, so their wall time is accumulated atomically.
	var msRestoreWall int64

	// Prepare: converge every node to the ucast echo profile. EnsureHostState is
	// idempotent, so nodes already correct cost one cheap state read and no
	// restart — we can therefore dispatch to all of them unconditionally instead
	// of guessing from a possibly-stale heartbeat.
	tPrepare := time.Now()
	o.hub.Emit("job", map[string]any{"status": "progress", "kind": "ucast", "variation": p.Variation,
		"phase": "prepare", "msg": fmt.Sprintf("converging %d node(s) to ucast echo profile", len(prep))})
	var prepWg sync.WaitGroup
	for _, n := range prep {
		prepWg.Add(1)
		go func(n registry.Node) {
			defer prepWg.Done()
			o.dispatchRetry(n.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
				Host: &proto.HostStateParams{Profile: proto.HostEchoUcast}}, 60*time.Second, 2)
		}(n)
	}
	prepWg.Wait()
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
	for _, n := range prep {
		purge.Add(1)
		go func(n registry.Node) {
			defer purge.Done()
			o.dispatchRetry(n.InstanceID, proto.Command{Type: proto.CmdPurgeDests}, 30*time.Second, 2)
		}(n)
	}
	purge.Wait()
	msPrepare = time.Since(tPrepare).Milliseconds()
	o.mu.Lock()
	o.lastMcastFwd = "" // a ucast run leaves replicators in ucast mode
	o.mu.Unlock()

	var done int64
	var rejected int64 // pairs refused by the loss gate (ran fine, numbers unusable)
	perPair := 30 * time.Second
	// MaxParallel caps concurrent pairs per source. 0 or negative = unlimited.
	maxPar := p.MaxParallel
	if maxPar <= 0 {
		// Effectively unlimited: no source has more dests than this.
		maxPar = 1
		for _, s := range nodes {
			if d := len(destsFor[s.InstanceID]); d > maxPar {
				maxPar = d
			}
		}
	}
	// AF_XDP TX binds a socket on a single TX queue, and every pair from a source
	// would bind the SAME queue. Concurrent binds contend and fall into rtt's
	// bind-retry backoff, which measured 2.2x SLOWER end-to-end than running them
	// serially (17.3s vs 7.8s over 6 pairs). Serialise instead.
	if xdpTx && maxPar > 1 {
		log.Printf("ucast/%s: forcing max_parallel 1 (was %d) — AF_XDP TX pairs share one queue and contend",
			p.Variation, maxPar)
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "ucast", "variation": p.Variation,
			"phase": "prepare", "msg": "AF_XDP TX shares one queue: running pairs serially (faster than concurrent)"})
		maxPar = 1
	}

	// Restores run asynchronously so a node's replicator startup (seconds of
	// AF_XDP bind + XDP attach) overlaps the next source's transition and
	// measurements instead of stalling the loop. restoreDone[instanceID] is
	// closed once that node can echo again; any source that needs it as a
	// destination waits on the channel first, so a measurement can never be
	// dispatched to a node that is still starting up.
	restoreDone := map[string]chan struct{}{}
	var restoreMu sync.Mutex
	awaitRestore := func(id string) {
		restoreMu.Lock()
		ch := restoreDone[id]
		restoreMu.Unlock()
		if ch != nil {
			<-ch
		}
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

		// This node is about to measure, so any outstanding restore of it must
		// finish first: it has to be fully stopped before we stop it again.
		awaitRestore(s.InstanceID)

		// This node becomes the measurer: stop its replicator so no AF_XDP
		// zero-copy socket owns the RX queue its echoes return on, and attach the
		// XDP program standalone so xdp mode still stamps. Grouping the matrix by
		// SOURCE keeps this to exactly TWO transitions per node for the whole
		// campaign; flipping per node-disjoint round would cost O(rounds x nodes)
		// systemctl operations instead.
		tClient := time.Now()
		hres, herr := o.dispatchRetry(s.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
			Host: &proto.HostStateParams{Profile: proto.HostClient, NeedXdpStamp: xdpRx}}, 60*time.Second, 2)
		msClientTransition += time.Since(tClient).Milliseconds()
		if herr != nil || !hres.OK {
			o.hub.Emit("job", map[string]any{"status": "progress", "kind": "ucast", "variation": p.Variation,
				"phase": "prepare", "ok": false, "src": s.PrivateIP,
				"err": "could not put node into client profile: " + firstErr(herr, hres.Err)})
			continue
		}

		var wg sync.WaitGroup
		sem := make(chan struct{}, maxPar)
		tMeasure := time.Now()

		// Order destinations so nodes with an outstanding restore are measured
		// LAST, giving their replicator the maximum time to finish starting while
		// the other pairs run.
		myDests := destsFor[s.InstanceID]
		dests := make([]registry.Node, 0, len(myDests))
		var restoring []registry.Node
		for _, d := range myDests {
			if d.InstanceID == s.InstanceID {
				continue
			}
			restoreMu.Lock()
			pending := restoreDone[d.InstanceID] != nil
			restoreMu.Unlock()
			if pending {
				restoring = append(restoring, d)
			} else {
				dests = append(dests, d)
			}
		}
		dests = append(dests, restoring...)

		for _, d := range dests {
			wg.Add(1)
			sem <- struct{}{} // block if maxPar measurements already in flight
			go func(s, d registry.Node) {
				defer wg.Done()
				defer func() { <-sem }()
				// Never measure to a node that is still starting its replicator.
				awaitRestore(d.InstanceID)
				cmd := proto.Command{Type: proto.CmdRunRTT, RTT: &proto.RTTParams{
					TargetIP: d.PrivateIP, DataPort: p.DataPort, ListenIP: s.PrivateIP, ListenPort: p.ListenPort,
					Count: p.Count, Rate: p.Rate, Warmup: p.Warmup, SendCPU: sendCPU, RecvCPU: recvCPU,
					XdpTx: xdpTx, XdpTxQueue: xdpTxQueue, XdpRx: xdpRx, MaxLossPct: p.MaxLossPct,
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
		msMeasure += time.Since(tMeasure).Milliseconds()

		// Restore this node to the echo profile so it can serve as a destination
		// for the remaining sources. Fired asynchronously: the replicator's
		// startup overlaps the next source's transition and measurements. The
		// channel is what later sources wait on before measuring to this node.
		ch := make(chan struct{})
		restoreMu.Lock()
		restoreDone[s.InstanceID] = ch
		restoreMu.Unlock()
		tRestore := time.Now()
		go func(n registry.Node, ch chan struct{}, started time.Time) {
			o.dispatchRetry(n.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
				Host: &proto.HostStateParams{Profile: proto.HostEchoUcast}}, 60*time.Second, 2)
			atomic.AddInt64(&msRestoreWall, time.Since(started).Milliseconds())
			restoreMu.Lock()
			delete(restoreDone, n.InstanceID)
			restoreMu.Unlock()
			close(ch)
		}(s, ch, tRestore)
	}

	// Drain any restore still in flight so the campaign does not report done
	// while a node is mid-restart.
	tDrain := time.Now()
	for {
		restoreMu.Lock()
		var ch chan struct{}
		for _, c := range restoreDone {
			ch = c
			break
		}
		restoreMu.Unlock()
		if ch == nil {
			break
		}
		<-ch
	}
	msRestore = atomic.LoadInt64(&msRestoreWall)
	msRestoreDrain := time.Since(tDrain).Milliseconds()
	rej := atomic.LoadInt64(&rejected)
	msTotal := time.Since(campaignStart).Milliseconds()
	timing := map[string]any{
		"total_ms":             msTotal,
		"prepare_ms":           msPrepare,
		"client_transition_ms": msClientTransition,
		"measure_ms":           msMeasure,
		"restore_ms":           msRestore,
		"restore_drain_ms":     msRestoreDrain,
	}
	o.hub.Emit("job", map[string]any{"status": "done", "kind": "ucast", "variation": p.Variation,
		"done": atomic.LoadInt64(&done), "total": total,
		"rejected_loss": rej, "max_loss_pct": p.MaxLossPct, "timing": timing})
	// pairs_ok excludes loss-gate rejections: those ran but produced no usable
	// numbers, so counting them would overstate the campaign's coverage.
	o.store.FinishRun(runID, int(atomic.LoadInt64(&done)-rej))
	log.Printf("TIMING ucast/%s total=%dms prepare=%dms client_transition=%dms measure=%dms restore=%dms drain=%dms overhead=%.1f%%",
		p.Variation, msTotal, msPrepare, msClientTransition, msMeasure, msRestore, msRestoreDrain,
		100*float64(msPrepare+msClientTransition+msRestore)/float64(max64(msTotal, 1)))
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

// max64 guards a division by zero in the timing percentage.
func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

// McastMatrixParams configures a multicast fan-out campaign across fwd modes.
type McastMatrixParams struct {
	Modes      []string `json:"modes"` // subset of copy|inplace|bpf_tx (default: all)
	Group      string   `json:"group"`
	DataPort   int      `json:"data_port"`
	Count      int      `json:"count"`
	IntervalUs int      `json:"interval_us"`
	TimeoutSec int      `json:"timeout_sec"`

	// Size is the mcast_send payload size in bytes (mirrors `-s`; tool minimum
	// 32B, tool default 64B). 0 => tool default. Matches mcast2ucast's
	// `--payload` sweep parameter, which af_xdp had no equivalent of before.
	Size int `json:"size,omitempty"`
	// TxQueue overrides mcast_send's AF_XDP TX queue (mirrors `-q`). 0 => tool
	// default (queue 1); queue 0 carries RSS/SSH traffic, see tools/README.md.
	TxQueue int `json:"tx_queue,omitempty"`
	// RxQueue overrides mcast_receive's AF_XDP/XDP queue index (mirrors `-q`).
	// 0 is both "unset" and the tool default.
	RxQueue int `json:"rx_queue,omitempty"`
}

// RunMcastMatrix drives the source -> replicator -> destination fan-out for each
// forward mode, encapsulating the proven setup: put the replicator in mcast
// mode+fwd, free the source/dest AF_XDP queues, (re)join destinations, gate on
// clock convergence, then run receivers concurrently with the source send.
//
// The fleet may have multiple online replicators (different PG/AZ placements).
// The campaign automatically sweeps every one of them - {replicator} x {mode} -
// so a single run compares latency across all replicator placements instead of
// only exercising whichever one the registry happened to return first. Each
// (replicator, mode) combination gets its own `runs` row tagging the
// replicator's identity/PG/AZ, so measurements stay attributable to the path
// that produced them. See dev/roadmap/ for the planned follow-up: a
// user-facing selector to run a subset of replicators instead of always all.
func (o *Orchestrator) RunMcastMatrix(p McastMatrixParams) {
	if !atomic.CompareAndSwapInt32(&o.running, 0, 1) {
		o.hub.Emit("job", map[string]string{"status": "rejected", "reason": "a campaign is already running"})
		return
	}
	defer atomic.StoreInt32(&o.running, 0)
	atomic.StoreInt32(&o.cancel, 0)

	if len(p.Modes) == 0 {
		p.Modes = []string{"copy", "inplace", "bpf_tx"}
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
	// Size/TxQueue/RxQueue are left at 0 when unset — RunMcastSend/RunMcastReceive
	// (and the C++ tools underneath) already treat 0 as "use the tool default",
	// so no explicit default assignment is needed here.

	source := o.reg.ByRole("source")
	replicators := o.reg.AllByRole("replicator")
	dests := o.reg.AllByRole("destination")
	if source == nil || len(replicators) == 0 || len(dests) == 0 {
		o.hub.Emit("job", map[string]any{"status": "error",
			"reason": fmt.Sprintf("need source+replicator+destination roles online (have source=%v replicators=%d dests=%d)",
				source != nil, len(replicators), len(dests))})
		return
	}
	replDesc := make([]string, len(replicators))
	for i, r := range replicators {
		replDesc[i] = fmt.Sprintf("%s(pg=%s,az=%s)", r.PrivateIP, r.PlacementGroup, r.AZ)
	}
	o.hub.Emit("job", map[string]any{"status": "running", "kind": "mcast",
		"modes": p.Modes, "source": source.PrivateIP, "replicators": replDesc, "dests": len(dests)})
	log.Printf("campaign mcast: source=%s replicators=%v dests=%d modes=%v",
		source.PrivateIP, replDesc, len(dests), p.Modes)

	mcastStart := time.Now()
	var msMTotal int64
	for ri, replicator := range replicators {
		if o.cancelled() {
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes})
			log.Printf("campaign mcast cancelled before replicator %s", replicator.PrivateIP)
			break
		}
		replicator := replicator
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast",
			"phase": "replicator", "replicator": replicator.PrivateIP,
			"replicator_pg": replicator.PlacementGroup, "replicator_az": replicator.AZ,
			"msg": fmt.Sprintf("replicator %d/%d: %s (pg=%s, az=%s)",
				ri+1, len(replicators), replicator.PrivateIP, replicator.PlacementGroup, replicator.AZ)})
		o.runMcastForReplicator(p, *source, replicator, dests)
	}
	msMTotal = time.Since(mcastStart).Milliseconds()
	o.hub.Emit("job", map[string]any{"status": "done", "kind": "mcast", "modes": p.Modes,
		"replicators": replDesc, "timing": map[string]any{"total_ms": msMTotal}})
	log.Printf("TIMING mcast total=%dms across %d replicator(s)", msMTotal, len(replicators))
}

// runMcastForReplicator runs the existing per-mode fan-out logic against ONE
// replicator. Split out of RunMcastMatrix so the replicator sweep loop stays
// readable; behavior for a single replicator is unchanged from before the sweep
// was added.
func (o *Orchestrator) runMcastForReplicator(p McastMatrixParams, source, replicator registry.Node, dests []registry.Node) {
	const svcT, runSetup = 45 * time.Second, 30 * time.Second

	// Phase timers so the done event reports where an mcast run spent its time.
	mcastStart := time.Now()
	var msMPrepare, msMSetMode, msMJoin, msMSettle, msMRun, msMCleanup int64

	// One-time: free the AF_XDP queue on the transient app nodes. Skip the (slow)
	// replicator STOP when it's already inactive from a prior run — but always run
	// the cheap cleanup (detach stale XDP / kill leftover procs) for robustness.
	// Converge source + destinations to the mcast endpoint profile: replicator
	// stopped and XDP detached, because mcast_send / mcast_receive bind their own
	// AF_XDP sockets and load their own program. EnsureHostState is idempotent, so
	// nodes already in that state cost one state read and no service work — this
	// replaces the previous heartbeat-guessing plus unconditional cleanup, and is
	// what makes a ucast -> mcast switchover converge in a single pass per node.
	tMPrep := time.Now()
	var mprep sync.WaitGroup
	endpoints := append([]registry.Node{source}, dests...)
	for _, n := range endpoints {
		mprep.Add(1)
		go func(n registry.Node) {
			defer mprep.Done()
			o.DispatchAgent(n.InstanceID, proto.Command{Type: proto.CmdEnsureHost,
				Host: &proto.HostStateParams{Profile: proto.HostMcastEndpoint}}, runSetup)
		}(n)
	}
	mprep.Wait()
	msMPrepare = time.Since(tMPrep).Milliseconds()
	o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "phase": "prepare",
		"replicator": replicator.PrivateIP, "msg": "freed AF_XDP queues on source + destinations"})

	for _, mode := range p.Modes {
		// Replicator into mcast fan-out with this fwd mode.
		if o.cancelled() {
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes,
				"replicator": replicator.PrivateIP})
			log.Printf("campaign mcast cancelled (replicator=%s)", replicator.PrivateIP)
			return
		}
		// bpf_tx (XDP_TX) mode is a single-destination passthrough - it cannot
		// fan out to multiple receivers. Use only the first destination as the
		// representative measurement; copy/inplace test the full fan-out.
		modeDests := dests
		if mode == "bpf_tx" && len(dests) > 1 {
			modeDests = dests[:1]
			o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
				"replicator": replicator.PrivateIP,
				"msg": fmt.Sprintf("bpf_tx mode: single-destination only (XDP_TX passthrough) - using %s", dests[0].PrivateIP)})
			log.Printf("mcast/bpf_tx: limiting to 1 destination (%s) - XDP_TX is single-dest passthrough", dests[0].PrivateIP)
		}

		// Anchor this (replicator, mode) combination in the runs table so its
		// measurements are attributable to the replicator path that produced
		// them. Telemetry arrives on the ingest goroutine, which reads the
		// current run id from the store - hence SetCurrentRun around the whole
		// mode run below.
		params := map[string]any{
			"replicator_id": replicator.InstanceID, "replicator_ip": replicator.PrivateIP,
			"replicator_pg": replicator.PlacementGroup, "replicator_az": replicator.AZ,
			"replicator_vpc": replicator.VpcID,
			"group": p.Group, "count": p.Count, "interval_us": p.IntervalUs,
		}
		runID, rErr := o.store.InsertRun("mcast", mode, "", "", len(modeDests), params)
		if rErr != nil {
			log.Printf("store: could not open run row for mcast/%s replicator=%s: %v", mode, replicator.PrivateIP, rErr)
		}
		o.store.SetCurrentRun(runID)

		// Clock sync does not depend on the replicator, so run it concurrently
		// with the mode switch (which restarts the replicator) instead of after.
		var clockWG sync.WaitGroup
		for _, n := range append([]registry.Node{source}, modeDests...) {
			clockWG.Add(1)
			go func(n registry.Node) {
				defer clockWG.Done()
				o.DispatchAgent(n.InstanceID, proto.Command{Type: proto.CmdClockSync}, runSetup)
			}(n)
		}

		// EnsureHostState is idempotent and collapses a mode+fwd change into a
		// single restart, so the cached-mode guard is only an extra fast path: if
		// the replicator is already in mcast/<mode> the agent does no service work.
		o.mu.Lock()
		skip := replicator.ReplicatorMode == "mcast" && o.lastMcastFwd == mode
		o.mu.Unlock()
		tMode := time.Now()
		if !skip {
			if res, err := o.DispatchAgent(replicator.InstanceID,
				proto.Command{Type: proto.CmdEnsureHost, Host: &proto.HostStateParams{
					Profile: proto.HostMcastReplicator, FwdMode: mode}}, svcT); err != nil || !res.OK {
				clockWG.Wait()
				o.hub.Emit("job", map[string]any{"status": "error", "mode": mode, "stage": "set_mode",
					"replicator": replicator.PrivateIP, "err": firstErr(err, res.Err)})
				o.store.FinishRun(runID, 0)
				o.store.SetCurrentRun(0)
				continue
			}
			o.mu.Lock()
			o.lastMcastFwd = mode
			o.mu.Unlock()
		}
		msMSetMode += time.Since(tMode).Milliseconds()
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
			"replicator": replicator.PrivateIP,
			"msg": "replicator in mcast/" + mode + " - destinations joining group + clock sync"})
		tJoin := time.Now()
		// Destinations (re)join the group behind the replicator. Joins need the
		// replicator listening so they follow the mode switch, but they are
		// independent of each other and run in parallel.
		var joinWG sync.WaitGroup
		for _, d := range modeDests {
			joinWG.Add(1)
			go func(d registry.Node) {
				defer joinWG.Done()
				o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdJoinGroup,
					Mcast: &proto.McastParams{ReplicatorIP: replicator.PrivateIP, Group: p.Group}}, runSetup)
			}(d)
		}
		joinWG.Wait()
		// Clock sync was started before the mode switch; collect it here.
		clockWG.Wait()
		msMJoin += time.Since(tJoin).Milliseconds()
		if o.cancelled() {
			for _, d := range modeDests {
				o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
			}
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes,
				"replicator": replicator.PrivateIP})
			log.Printf("campaign mcast cancelled during %s setup (replicator=%s)", mode, replicator.PrivateIP)
			o.store.FinishRun(runID, 0)
			o.store.SetCurrentRun(0)
			return
		}
		o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
			"replicator": replicator.PrivateIP,
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
			tRun := time.Now()
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
							Variation: mode, RxQueue: p.RxQueue}}, recvT)
					results[i] = rr{d.PrivateIP, res, err}
				}()
			}
			// Wait for every receiver to have attached its XDP program and bound
			// its socket before the source starts sending. Polling the receivers'
			// own "listening" signal replaces a blind 3s sleep that was ~47% of a
			// single-mode run. The 3s cap means this is never slower than the
			// sleep it replaces, and a receiver that never reports ready still
			// gets the send (its own timeout then surfaces the failure).
			tSettle := time.Now()
			settleDeadline := time.Now().Add(3 * time.Second)
			for time.Now().Before(settleDeadline) {
				allReady := true
				for _, d := range modeDests {
					res, err := o.DispatchAgent(d.InstanceID,
						proto.Command{Type: proto.CmdMcastRxReady}, 3*time.Second)
					if err != nil || !res.OK {
						allReady = false
						break
					}
				}
				if allReady {
					break
				}
				time.Sleep(50 * time.Millisecond)
			}
			atomic.AddInt64(&msMSettle, time.Since(tSettle).Milliseconds())
			sres, serr := o.DispatchAgent(source.InstanceID, proto.Command{Type: proto.CmdMcastSend,
				Mcast: &proto.McastParams{Group: p.Group, DataPort: p.DataPort, ReplicatorIP: replicator.PrivateIP,
					Count: p.Count, IntervalUs: p.IntervalUs, Size: p.Size, TxQueue: p.TxQueue}}, recvT)
			wg.Wait()
			atomic.AddInt64(&msMRun, time.Since(tRun).Milliseconds())
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
				log.Printf("mcast/%s attempt %d failed (replicator=%s); retrying", mode, attempt, replicator.PrivateIP)
				for _, d := range modeDests {
					o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
				}
			}
		}
		if o.cancelled() {
			for _, d := range modeDests {
				o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
			}
			o.hub.Emit("job", map[string]any{"status": "cancelled", "kind": "mcast", "modes": p.Modes,
				"replicator": replicator.PrivateIP})
			log.Printf("campaign mcast cancelled during %s run (replicator=%s)", mode, replicator.PrivateIP)
			o.store.FinishRun(runID, 0)
			o.store.SetCurrentRun(0)
			return
		}
		pairsOK := 0
		for _, r := range results {
			pairOK := r.err == nil && r.res.OK
			if pairOK {
				pairsOK++
			}
			o.hub.Emit("job", map[string]any{"status": "progress", "kind": "mcast", "mode": mode,
				"src": source.PrivateIP, "dst": r.dst, "replicator": replicator.PrivateIP,
				"ok": pairOK, "err": firstErr(r.err, r.res.Err)})
		}
		o.store.FinishRun(runID, pairsOK)
		o.store.SetCurrentRun(0)
		tClean := time.Now()
		for _, d := range modeDests { // release the queue for the next mode
			o.DispatchAgent(d.InstanceID, proto.Command{Type: proto.CmdCleanup}, runSetup)
		}
		msMCleanup += time.Since(tClean).Milliseconds()
		o.hub.Emit("job", map[string]any{"status": "mode_done", "kind": "mcast", "mode": mode,
			"replicator": replicator.PrivateIP, "ok": ok})
		log.Printf("campaign mcast/%s done (ok=%v, replicator=%s)", mode, ok, replicator.PrivateIP)
	}
	msMTotal := time.Since(mcastStart).Milliseconds()
	o.hub.Emit("job", map[string]any{"status": "replicator_done", "kind": "mcast", "modes": p.Modes,
		"replicator": replicator.PrivateIP,
		"timing": map[string]any{"total_ms": msMTotal, "prepare_ms": msMPrepare,
			"set_mode_ms": msMSetMode, "join_ms": msMJoin, "settle_ms": msMSettle,
			"run_ms": msMRun, "cleanup_ms": msMCleanup}})
	log.Printf("TIMING mcast/%v replicator=%s total=%dms prepare=%dms set_mode=%dms join=%dms settle=%dms run=%dms cleanup=%dms",
		p.Modes, replicator.PrivateIP, msMTotal, msMPrepare, msMSetMode, msMJoin, msMSettle, msMRun, msMCleanup)
}
