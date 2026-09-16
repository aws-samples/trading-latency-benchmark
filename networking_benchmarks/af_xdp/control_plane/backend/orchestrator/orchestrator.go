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
	"afxdp-cp/backend/wander"
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

// nicTuningBaseline is the tuning bake-ami.sh applies fleet-wide, and the only
// state under which the measured error terms in dev/roadmap's precision
// design (e.g. b_rx ~107ns) are valid. Off this baseline, ENA's ingress path
// reverts to interrupt-driven delivery and untuned figures on the same
// hardware class were measured at -12 to -390us - two to four orders of
// magnitude larger, and NOT comparable to a tuned run.
//
// "" for a key means: accept any non-empty value (covers rx_queues_current/
// tx_queues_current, which report "n/a" by design on ENA - see NicTuning's
// own comment - and combined_queues_current, whose expected value is
// instance-size-dependent rather than fixed).
var nicTuningBaseline = map[string]string{
	"napi_defer_hard_irqs": "2",
	"gro_flush_timeout":    "10000",
	"rx_usecs":             "0",
	"tx_usecs":             "0",
	"adaptive_rx":          "off",
}

// nicTuningViolations compares one node's NicTuning reading against
// nicTuningBaseline and returns a human-readable violation per mismatched
// key, prefixed with role and IP so a multi-node report attributes each
// drift to its node. A key ABSENT from tuning (agent didn't report it) is
// not a violation - that is the older-agent best-effort path, distinct from
// a CURRENT agent reporting a value that differs from baseline.
func nicTuningViolations(role, ip string, tuning map[string]string) []string {
	var out []string
	for key, want := range nicTuningBaseline {
		got, present := tuning[key]
		if !present {
			continue
		}
		if got != want {
			out = append(out, fmt.Sprintf("%s(%s) %s=%q want %q", role, ip, key, got, want))
		}
	}
	return out
}

// wanderTargets returns the nodes a wander sampler should run on for one
// (replicator, mode) run, per wander-sampler-lifecycle-design.md §3 step 2:
// source, replicator, and every destination in modeDests - not just the
// reduced tuningNodes sample the NIC-tuning gate uses, since the wander band
// needs per-node coverage for whichever nodes' clocks are relevant to the
// reported figure. Deduplicated by InstanceID so a degenerate topology
// (source==replicator, or a destination list containing the source) never
// dispatches two wander_start commands to the same node.
func wanderTargets(source, replicator registry.Node, modeDests []registry.Node) []registry.Node {
	seen := map[string]bool{}
	var out []registry.Node
	add := func(n registry.Node) {
		if seen[n.InstanceID] {
			return
		}
		seen[n.InstanceID] = true
		out = append(out, n)
	}
	add(source)
	add(replicator)
	for _, d := range modeDests {
		add(d)
	}
	return out
}

// wanderRunSeconds is the sampler's own lifetime bound for one settle+run
// attempt, per wander-band-design.md §3's "campaign duration + 30s margin"
// (the margin itself is applied inside StartWander/Runner - this is only the
// "campaign duration" half, i.e. the run's own worst-case wall-clock span).
// TimeoutSec bounds mcast_receive; the retry/settle overhead on top of it is
// small and constant, so a flat +10s covers it without needing a second
// configurable parameter.
func wanderRunSeconds(timeoutSec int) int {
	return timeoutSec + 10
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
	Modes      []string `json:"modes"` // subset of copy|inplace|kernel (default: all)
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

	// WanderSample toggles the clock-wander sampler (dev/roadmap/precision/
	// wander-band-design.md, wander-sampler-lifecycle-design.md) for this
	// campaign. Default false: the sampler is opt-in, per-run, and never a
	// persistent background process - see wander-sampler-lifecycle-design.md
	// §2.4/§4 for why it is scoped to exactly one (replicator, mode) run's
	// settle+run window rather than running continuously.
	WanderSample bool `json:"wander_sample,omitempty"`
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
		p.Modes = []string{"copy", "inplace"}
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
		modeDests := dests

		// Anchor this (replicator, mode) combination in the runs table so its
		// measurements are attributable to the replicator path that produced
		// them. Telemetry arrives on the ingest goroutine, which reads the
		// current run id from the store - hence SetCurrentRun around the whole
		// mode run below.
		params := map[string]any{
			"replicator_id": replicator.InstanceID, "replicator_ip": replicator.PrivateIP,
			"replicator_pg": replicator.PlacementGroup, "replicator_az": replicator.AZ,
			"replicator_vpc": replicator.VpcID,
			"group":          p.Group, "count": p.Count, "interval_us": p.IntervalUs,
			"size": p.Size, "tx_queue": p.TxQueue, "rx_queue": p.RxQueue,
		}
		// NIC tuning state (dev/roadmap precision design P11: "promote from
		// metadata to gate"). Read from source/replicator/first destination -
		// enough to catch a fleet node that drifted from the baked baseline
		// without dispatching to every destination in a large fan-out. Prefix
		// each node's keys with its role so all three show up distinctly in
		// one flat params map instead of colliding.
		tuningNodes := map[string]registry.Node{"src": source, "repl": replicator}
		if len(modeDests) > 0 {
			tuningNodes["dst"] = modeDests[0]
		}
		var tuningWG sync.WaitGroup
		var tuningMu sync.Mutex
		var tuningViolations []string
		for role, n := range tuningNodes {
			tuningWG.Add(1)
			go func(role string, n registry.Node) {
				defer tuningWG.Done()
				res, err := o.DispatchAgent(n.InstanceID, proto.Command{Type: proto.CmdNicTuning}, runSetup)
				if err != nil || !res.OK || res.NicTuning == nil {
					// Best-effort for OLDER agents that predate CmdNicTuning:
					// contribute no keys and no violation, rather than failing
					// a run whose agent simply doesn't answer this command.
					// This does NOT cover a CURRENT agent reporting drifted
					// values - that path below always evaluates the gate.
					return
				}
				tuningMu.Lock()
				for k, v := range res.NicTuning {
					params["nic_"+role+"_"+k] = v
				}
				if v := nicTuningViolations(role, n.PrivateIP, res.NicTuning); len(v) > 0 {
					tuningViolations = append(tuningViolations, v...)
				}
				tuningMu.Unlock()
			}(role, n)
		}
		tuningWG.Wait()
		// Gate: a node whose NIC tuning drifted from the baked baseline has a
		// different bias profile (b_rx is a property of the tuning, not the
		// NIC - untuned measured -12 to -390us vs ~107ns tuned) and its
		// measurements are not comparable to a correctly-tuned run. Reject
		// rather than record a plausible-looking but incomparable result.
		if len(tuningViolations) > 0 {
			o.hub.Emit("job", map[string]any{"status": "error", "mode": mode,
				"stage": "nic_tuning_gate", "replicator": replicator.PrivateIP,
				"err": strings.Join(tuningViolations, "; ")})
			log.Printf("campaign mcast/%s REJECTED (replicator=%s): nic tuning drift: %s",
				mode, replicator.PrivateIP, strings.Join(tuningViolations, "; "))
			continue
		}
		runID, rErr := o.store.InsertRun("mcast", mode, "", "", len(modeDests), params)
		if rErr != nil {
			log.Printf("store: could not open run row for mcast/%s replicator=%s: %v", mode, replicator.PrivateIP, rErr)
		}
		o.store.SetCurrentRun(runID)

		// Clock sync runs concurrently with the mode switch (which restarts the
		// replicator) rather than after it, since chronyd is independent of
		// replicator.service. clockWG is waited on before any traffic starts.
		//
		// The REPLICATOR MUST BE INCLUDED here. It stamps replicator_ns (the hop1
		// endpoint) and replicator_tx_ns (the hop2 start), i.e. two of the four
		// timestamps in a one-way mcast measurement. Leaving it unsynced was a real
		// defect: a freshly started node can sit ~0.6-0.9 s off the PHC because
		// chrony's `makestep 1.0 3` declines to step a sub-1s offset and slews it
		// off at maxslewrate 500 instead (~27 min to converge). With the replicator
		// skipped that produced the bogus hop1=0 / hop2~=665000us runs recorded in
		// dev/roadmap/fix.md. CmdClockSync issues an explicit `chronyc makestep`,
		// which ignores the 1.0 s threshold. See dev/roadmap/precision.md Finding 2.
		var clockWG sync.WaitGroup
		for _, n := range append([]registry.Node{source, replicator}, modeDests...) {
			clockWG.Add(1)
			go func(n registry.Node) {
				defer clockWG.Done()
				// Surface a failed convergence instead of discarding it. ClockSync
				// now gates on `chronyc tracking` System time (see clockOffsetMaxUs
				// in agent/runner.go), so a non-OK result here means this node's
				// clock did not converge and its timestamps cannot be trusted for a
				// one-way split. Emitted as a warning rather than aborting the mode:
				// the per-destination clock-skew gate in RunMcastReceive is the hard
				// stop, and this tells the operator which node to look at first.
				res, err := o.DispatchAgent(n.InstanceID, proto.Command{Type: proto.CmdClockSync}, runSetup)
				if err != nil || !res.OK {
					detail := firstErr(err, res.Err)
					log.Printf("clock_sync: node %s (%s) did not converge: %s",
						n.PrivateIP, n.InstanceID, detail)
					o.hub.Emit("job", map[string]any{"status": "warn", "kind": "mcast", "mode": mode,
						"stage": "clock_sync", "node": n.PrivateIP, "err": detail})
				}
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
			"msg":        "replicator in mcast/" + mode + " - destinations joining group + clock sync"})
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
			"msg":        fmt.Sprintf("sending %d packets source→replicator→%d dest(s)", p.Count, len(modeDests))})

		// Wander sampler targets for this mode's attempts (wander-sampler-
		// lifecycle-design.md §3 step 2) - computed once per mode since
		// modeDests is fixed across retries. Empty/unused entirely when
		// p.WanderSample is false: StartWander/StopWander are only ever
		// dispatched inside the `if p.WanderSample` guards below, so a
		// campaign with the flag off issues zero wander_start/wander_stop
		// commands and pays zero extra NATS round-trips.
		wanderNodes := wanderTargets(source, replicator, modeDests)
		wanderSeconds := wanderRunSeconds(p.TimeoutSec)

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
		// wanderCSVs accumulates the LAST attempt's collected sampler output
		// per node - overwritten (not appended) on each retry so a failed
		// first attempt's stale CSV never gets merged alongside a
		// successful second attempt's; only the attempt that actually
		// produced the stored run's results should contribute wander data.
		// wanderStartSnap/wanderEndSnap hold G4/G5's per-node evidence from
		// the SAME attempt as wanderCSVs, for the same reason.
		wanderCSVs := map[string]string{}
		wanderStartSnap := map[string]wander.PhcCounterSnapshot{}
		wanderEndSnap := map[string]wander.PhcCounterSnapshot{}
		wanderRefclockOK := map[string]bool{}  // per node: G5's ok (agent answered at all)
		wanderRefclockSel := map[string]bool{} // per node: G5's selected value
		for attempt := 1; attempt <= 2 && !ok; attempt++ {
			if o.cancelled() {
				break
			}
			tRun := time.Now()
			var wg sync.WaitGroup
			results = make([]rr, len(modeDests))

			// Start the wander sampler BEFORE the settle-phase readiness poll
			// below, per wander-sampler-lifecycle-design.md §2.2/§3: it must
			// span settle+run, not just run, so the samples nearest the
			// start of the attempt (while receivers are still confirming
			// readiness) still have concurrent clock evidence. Best-effort
			// per node - a node whose agent predates CmdWanderStart, or
			// whose PTP device is unavailable, must not fail the traffic
			// run; the reduction stage's own gates (G1-G5) are what reject
			// an unusable sampler window, not this dispatch.
			if p.WanderSample {
				var startWG sync.WaitGroup
				var startMu sync.Mutex
				for _, n := range wanderNodes {
					n := n
					startWG.Add(1)
					go func() {
						defer startWG.Done()
						res, err := o.DispatchAgent(n.InstanceID, proto.Command{Type: proto.CmdWanderStart,
							Wander: &proto.WanderParams{Seconds: wanderSeconds}}, runSetup)
						if err != nil || !res.OK {
							log.Printf("wander_start: node %s (%s) did not start: %s",
								n.PrivateIP, n.InstanceID, firstErr(err, res.Err))
						}
						// Capture G4/G5 evidence regardless of whether the
						// sampler itself started - a failed start is still
						// useful to correlate against a bad refclock/counter
						// state, and the gates fail closed on missing data
						// either way.
						startMu.Lock()
						if res.PhcCounters != nil {
							wanderStartSnap[n.PrivateIP] = wander.PhcCounterSnapshot(res.PhcCounters)
						}
						if res.RefclockPhcSelected != nil {
							wanderRefclockOK[n.PrivateIP] = true
							wanderRefclockSel[n.PrivateIP] = *res.RefclockPhcSelected
						}
						startMu.Unlock()
					}()
				}
				// Awaited (not fire-and-forget) so G4/G5's window-start
				// evidence is guaranteed captured before the settle phase
				// begins - a race here would mean a node's start-snapshot
				// might arrive after its stop-snapshot, corrupting the
				// bracket the gate is supposed to check.
				startWG.Wait()
			}
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
							// IntervalUs is the sender's pacing, not consumed by
							// mcast_receive's own CLI flags - it is forwarded here
							// purely so RunMcastReceive can compute RequestedPps
							// (dev/roadmap/fix.md's "Report achieved vs requested rate"
							// item). Omitting it left RequestedPps/RateShortfall
							// permanently unset for every mcast run - caught live
							// when a real run showed achieved_pps populated but
							// requested_pps/rate_shortfall always nil.
							IntervalUs: p.IntervalUs,
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
					Count: p.Count, IntervalUs: p.IntervalUs, Size: p.Size, TxQueue: p.TxQueue, Variation: mode}}, recvT)
			wg.Wait()
			atomic.AddInt64(&msMRun, time.Since(tRun).Milliseconds())
			// Stop the wander sampler right after the run phase closes -
			// wander-sampler-lifecycle-design.md §3 step 8. Collected here
			// (not deferred to after the retry loop) so a RETRIED attempt's
			// stale CSV is overwritten by this attempt's, matching the
			// "last attempt wins" comment on wanderCSVs above. Best-effort:
			// a node that never started (agent too old, PTP unavailable) or
			// whose stop call fails just contributes no CSV for this
			// attempt - it does not fail the traffic run, which has
			// already completed by this point regardless.
			if p.WanderSample {
				var wanderWG sync.WaitGroup
				var wanderMu sync.Mutex
				for _, n := range wanderNodes {
					n := n
					wanderWG.Add(1)
					go func() {
						defer wanderWG.Done()
						res, err := o.DispatchAgent(n.InstanceID, proto.Command{Type: proto.CmdWanderStop}, runSetup)
						if err != nil || !res.OK {
							log.Printf("wander_stop: node %s (%s) failed: %s",
								n.PrivateIP, n.InstanceID, firstErr(err, res.Err))
							return
						}
						wanderMu.Lock()
						wanderCSVs[n.PrivateIP] = res.WanderCSV
						if res.PhcCounters != nil {
							wanderEndSnap[n.PrivateIP] = wander.PhcCounterSnapshot(res.PhcCounters)
						}
						wanderMu.Unlock()
					}()
				}
				wanderWG.Wait()
			}
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
		// Reduce + gate the wander sampler's collected CSVs, if any
		// (wander-sampler-lifecycle-design.md §3 step 9, wander-band-design.md
		// §4/§5). MergeRunParams rather than a second InsertRun call - the
		// run row and its params were already opened at the top of this
		// mode, before the sampler's data existed. Both the raw CSV and the
		// reduced+gated fields are stored: raw so the band can be
		// recomputed under a different model without re-running (§4), and
		// reduced so a reader does not have to re-parse CSV text to see the
		// figures. Keyed "wander_csv_<ip>"/"wander_<ip>_<field>" per node.
		if p.WanderSample && len(wanderCSVs) > 0 {
			extra := make(map[string]any, len(wanderCSVs)*8)
			// reducedByIP holds each node's Reduce() output (keyed by IP) so
			// the pairwise band (wander-band-design.md §4's PairwiseBand,
			// sqrt(madA^2+madC^2)) can be computed for source<->destination
			// pairs after every node's own fields are known - PairwiseBand
			// needs two nodes' WanderMadScaledNs together, not one node's
			// fields in isolation, so it cannot be computed inside the
			// per-node loop below.
			reducedByIP := make(map[string]wander.Fields, len(wanderCSVs))
			rejectedByIP := make(map[string]bool, len(wanderCSVs))
			for ip, csv := range wanderCSVs {
				extra["wander_csv_"+ip] = csv
				samples, perr := wander.ParseCSV(strings.NewReader(csv))
				if perr != nil {
					extra["wander_"+ip+"_error"] = fmt.Sprintf("parse: %v", perr)
					continue
				}
				f := wander.Reduce(samples)
				reducedByIP[ip] = f
				extra["wander_"+ip+"_med_ns"] = f.WanderMedNs
				extra["wander_"+ip+"_mad_raw_ns"] = f.WanderMadRawNs
				extra["wander_"+ip+"_mad_scaled_ns"] = f.WanderMadScaledNs
				extra["wander_"+ip+"_mad_per_sqrt_sec_ns"] = f.WanderMadPerSqrtSecNs
				extra["wander_"+ip+"_duration_sec"] = f.WanderDurationSec
				extra["wander_"+ip+"_p1_ns"] = f.WanderP1Ns
				extra["wander_"+ip+"_p99_ns"] = f.WanderP99Ns
				extra["wander_"+ip+"_excursion_pct"] = f.WanderExcursionPct
				extra["wander_"+ip+"_step_count"] = f.WanderStepCount
				extra["wander_"+ip+"_bracket_med_ns"] = f.BracketMedNs
				extra["wander_"+ip+"_eb_med_ns"] = f.EbMedNs
				extra["wander_"+ip+"_sample_count"] = f.SampleCount
				extra["wander_"+ip+"_sentinel_count"] = f.SentinelCount

				// Gates G1-G6 (wander-band-design.md §5). A REJECT gate
				// failing is recorded as a violation (surfaced the same way
				// nic_tuning_gate violations are, via the job event and
				// params, not by discarding the traffic result itself -
				// the wander band is metadata about the measurement, and
				// the design doc's own gates are reject-only for the BAND,
				// never a retroactive correction of the recorded latency).
				var violations []string
				if ok, reason := wander.GateSampleCoverage(f, wander.ExpectedSamples(10, wander.ActualDurationSec(samples))); !ok {
					violations = append(violations, reason)
				}
				if ok, reason := wander.GateSentinelRate(f); !ok {
					violations = append(violations, reason)
				}
				if ok, reason := wander.GateNoStep(f); !ok {
					violations = append(violations, reason)
				}
				if ok, reason := wander.GatePhcCountersClean(wanderStartSnap[ip], wanderEndSnap[ip]); !ok {
					violations = append(violations, reason)
				}
				if ok, reason := wander.GateRefclockIsPhc(wanderRefclockSel[ip], wanderRefclockOK[ip]); !ok {
					violations = append(violations, reason)
				}
				if flag, reason := wander.GateExcursionNorm(f); flag {
					extra["wander_"+ip+"_excursion_flag"] = reason
				}
				if len(violations) > 0 {
					extra["wander_"+ip+"_rejected"] = strings.Join(violations, "; ")
					rejectedByIP[ip] = true
					log.Printf("wander gates rejected node %s (mode=%s replicator=%s): %s",
						ip, mode, replicator.PrivateIP, strings.Join(violations, "; "))
				}
			}

			// Pairwise band (wander-band-design.md §4): for every source<->
			// destination pair with BOTH nodes' wander evidence gate-clean,
			// combine their scaled MADs in quadrature. Only source<->dest
			// pairs are computed, not source<->replicator or replicator<->
			// dest - the reported one-way figure this band qualifies is
			// rx_ns-ts_ns (source to destination, end to end; src/README.md's
			// "How multicast latency is measured"), so the band must describe
			// noise on exactly that pair, not an intermediate hop. A pair with
			// either side rejected is skipped entirely (PairwiseBand.md and
			// wander.go's own doc comment: quadrature is only valid combining
			// two genuine sigma-equivalent MADs; a rejected node's MAD is not
			// trustworthy evidence and must not be silently folded in).
			var maxBandNs float64
			haveBand := false
			if srcF, ok := reducedByIP[source.PrivateIP]; ok && !rejectedByIP[source.PrivateIP] {
				for _, d := range modeDests {
					dstF, ok := reducedByIP[d.PrivateIP]
					if !ok || rejectedByIP[d.PrivateIP] {
						continue
					}
					band := wander.PairwiseBand(srcF.WanderMadScaledNs, dstF.WanderMadScaledNs)
					extra["wander_pairwise_band_"+source.PrivateIP+"_"+d.PrivateIP+"_ns"] = band
					if !haveBand || band > maxBandNs {
						maxBandNs = band
						haveBand = true
					}
				}
			}
			if haveBand {
				// wander_pairwise_band_max_ns is the single figure a reader
				// wants at a glance for this run: the worst-case (largest)
				// band across every gate-clean source<->destination pair,
				// so a multi-destination run is never represented by an
				// optimistic pair while a noisier one goes unnoticed.
				extra["wander_pairwise_band_max_ns"] = maxBandNs
			}
			if err := o.store.MergeRunParams(runID, extra); err != nil {
				log.Printf("store: could not merge wander data into run %d (mode=%s replicator=%s): %v",
					runID, mode, replicator.PrivateIP, err)
			}
		}
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
