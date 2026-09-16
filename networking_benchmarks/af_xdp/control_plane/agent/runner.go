package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"afxdp-cp/proto"
)

// Runner executes the C++ measurement tools and owns the node-local resource
// lifecycle. The reproducibility fixes learned the hard way (free the AF_XDP
// queue before/after a run; makestep the clock; report skew) live HERE, so the
// backend never micromanages them over the wire — it just issues intents.
type Runner struct {
	binDir string // e.g. /opt/af-xdp

	// wanderMu guards the wander sampler's process handle. Deliberately a
	// SEPARATE lock from the agent's execMu (main.go): StartWander must
	// return immediately so the sampler runs CONCURRENTLY with whatever
	// commands drive the settle+run window it's meant to cover
	// (wander-sampler-lifecycle-design.md §2.2/§3) — serializing it behind
	// execMu the way CmdMcastReceive is serialized would block every other
	// command for the sampler's whole lifetime, which is exactly backwards.
	wanderMu     sync.Mutex
	wanderCancel context.CancelFunc
	wanderDone   chan struct{} // closed when the child process's Wait() returns
	wanderOut    bytes.Buffer  // collected stdout, read only after wanderDone closes
	wanderErr    error         // wait error, read only after wanderDone closes

	// wanderCmd builds the sampler's exec.Cmd for a given context+duration.
	// Defaults to the real phcsample invocation (set in NewRunner); tests
	// substitute a fake short-lived binary here so the bounded-lifetime
	// property (StartWander/StopWander, context cancellation) can be proven
	// without real PTP hardware or root privileges.
	wanderCmd func(ctx context.Context, seconds int) (*exec.Cmd, error)
}

// NewRunner creates a Runner that drives the C++ measurement tools in binDir.
func NewRunner(binDir string) *Runner {
	r := &Runner{binDir: binDir}
	r.wanderCmd = r.realWanderCmd
	return r
}

func (r *Runner) bin(name string) string { return r.binDir + "/" + name }

// sh runs a bash snippet, returning combined output. Uses a non-login shell:
// the scripts reference absolute paths, so sourcing /etc/profile and every
// profile.d entry on each of the ~hundreds of calls per campaign is pure cost.
func sh(script string) (string, error) {
	out, err := exec.Command("bash", "-c", script).CombinedOutput()
	return string(out), err
}

func iface() string {
	out, _ := sh(`ip -4 route show default | awk '{print $5}' | head -1`)
	return strings.TrimSpace(out)
}

func readIsolcpus() string {
	b, err := os.ReadFile("/proc/cmdline")
	if err != nil {
		return ""
	}
	for _, tok := range strings.Fields(string(b)) {
		if strings.HasPrefix(tok, "isolcpus=") {
			return strings.TrimPrefix(tok, "isolcpus=")
		}
	}
	return ""
}

// clockOffsetUs reports how far this node's system clock is from its reference,
// absolute, in microseconds. -1 if it cannot be determined.
//
// Reads chronyc tracking's "System time", NOT "Last offset". "Last offset"/
// "RMS offset" describe how well chrony tracks the rate of its reference; they
// say nothing about the absolute error still outstanding on the system clock,
// which can be hundreds of milliseconds off while those fields read
// nanoseconds (e.g. while chrony has locked the reference frequency but is
// still slewing off a sub-1s step rather than stepping it). Gating on those
// fields instead would admit garbage one-way measurements during that window.
func clockOffsetUs() float64 {
	// "System time : 0.814552128 seconds slow of NTP time" -> field 4 is the value.
	out, err := sh(`chronyc tracking 2>/dev/null | awk -F': *' '/^System time/{print $2}' | awk '{print $1}'`)
	if err != nil {
		return -1
	}
	v, err := strconv.ParseFloat(strings.TrimSpace(out), 64)
	if err != nil {
		return -1
	}
	return math.Abs(v) * 1e6
}

// ClockSync forces convergence (makestep + burst) and returns the achieved
// offset, as clockOffsetUs measures it (system-clock error, not rate-tracking
// quality). makestep is what rescues the sub-1s post-boot offset that
// chrony's own `makestep 1.0 3` threshold declines to correct.
func (r *Runner) ClockSync() (float64, error) {
	if _, err := sh(`chronyc makestep >/dev/null 2>&1 || true; chronyc burst 3/4 >/dev/null 2>&1 || true;
		for i in $(seq 1 10); do chronyc tracking 2>/dev/null | grep -q "Leap status *: *Normal" && break; sleep 1; done`); err != nil {
		return -1, err
	}
	off := clockOffsetUs()
	// CONVERGENCE GATE - makestep above is best-effort (`|| true`), so a failed
	// step, an unhealthy chronyd, or a missing PHC refclock would otherwise pass
	// silently and the caller would run a one-way measurement on a bad clock.
	// Fail loudly instead. clockOffsetUs reads `System time`, i.e. the absolute
	// error still outstanding on the system clock - NOT `Last offset`/`RMS
	// offset`, which report rate-tracking quality and read as nanoseconds even
	// when the clock is hundreds of milliseconds wrong.
	//
	// Threshold rationale: converged nodes on this fleet measure 6-128 ns of
	// System time, and the irreducible floor (phc_error_bound) is 14-30 us, so
	// 100 us is ~1000x the healthy value while staying far above any legitimate
	// steady-state reading. It is a broken-clock detector, not a precision knob.
	// -1 means chronyc was unreadable, which is itself a failure worth surfacing.
	if off < 0 {
		return off, fmt.Errorf("clock sync: could not read chronyc tracking System time")
	}
	if off > clockOffsetMaxUs {
		return off, fmt.Errorf(
			"clock sync: System time is %.1f us off after makestep (max %.0f us) - "+
				"clock did not converge; check `chronyc tracking` and that the PHC refclock is selected",
			off, float64(clockOffsetMaxUs))
	}
	return off, nil
}

// clockOffsetMaxUs is the ClockSync convergence threshold. See ClockSync.
const clockOffsetMaxUs = 100.0

// NicTuning reads the NAPI/interrupt-coalescing tuning state bake-ami.sh sets
// (see ena-rx-lowlat.service / ena-coalescing.service) on this node's default
// interface, and returns it as a flat map ready to merge into a run's params.
// A run whose tuning differs from the baked baseline (napi_defer_hard_irqs=2,
// gro_flush_timeout=10000, rx-usecs=0, tx-usecs=0, adaptive-rx off) is not
// comparable to one that matches it; recording this state as part of the run
// avoids needing a manual SSH+sysfs-read to check after the fact.
// Best-effort throughout: a value this node's kernel/driver doesn't expose
// (e.g. napi_defer_hard_irqs on a non-ENA NIC) is reported as "" rather than
// failing the whole read, since a partial tuning snapshot is still useful and
// the caller (orchestrator) only needs this for comparability/debugging, not
// as a gate on the run itself.
func (r *Runner) NicTuning() map[string]string {
	nic := iface()
	out := map[string]string{"iface": nic}
	if nic == "" {
		return out
	}
	readSysfs := func(key, path string) {
		b, err := os.ReadFile(path)
		if err != nil {
			out[key] = ""
			return
		}
		out[key] = strings.TrimSpace(string(b))
	}
	readSysfs("napi_defer_hard_irqs", "/sys/class/net/"+nic+"/napi_defer_hard_irqs")
	readSysfs("gro_flush_timeout", "/sys/class/net/"+nic+"/gro_flush_timeout")

	// ethtool -c: coalescing (rx-usecs/tx-usecs/adaptive-rx). ethtool -g: ring
	// sizes / RSS indirection isn't in -g, so report the queue count from -l
	// instead (combined channel count - ENA doesn't support combined=1, so
	// this fleet always shows separate rx/tx, which is itself worth recording
	// since the RSS-indirection-to-queue-0 setup (ena-xdp-queues.service)
	// depends on that being true).
	if coalesce, err := sh(fmt.Sprintf("ethtool -c %s 2>/dev/null", nic)); err == nil {
		for _, want := range []string{"rx-usecs:", "tx-usecs:"} {
			out[nicTuningKey(want)] = nicTuningGrep(coalesce, want)
		}
		// "Adaptive RX: off  TX: n/a" is ONE line carrying both values, not
		// two separate "Adaptive RX:"/"Adaptive TX:" lines - a plain
		// line-prefix grep for "Adaptive TX:" never matches, and one for
		// "Adaptive RX:" pulls in the trailing "TX: n/a" too. Parse the pair
		// off that single line explicitly instead.
		rx, tx := nicTuningAdaptive(coalesce)
		out["adaptive_rx"] = rx
		out["adaptive_tx"] = tx
	}
	if channels, err := sh(fmt.Sprintf("ethtool -l %s 2>/dev/null", nic)); err == nil {
		// -l prints "Current hardware settings" after "Pre-set maximums" -
		// take the value from the SECOND occurrence of each field, matching
		// what the NIC is actually running with, not what it merely supports.
		// rx_queues_current/tx_queues_current read "n/a" on this fleet's ENA
		// driver by design, not a parse failure: ENA doesn't support
		// combined=1 (see bake-ami.sh's ena-xdp-queues.service comment) and
		// only ever reports queue count under "Combined:".
		out["rx_queues_current"] = nicTuningSecond(channels, "RX:")
		out["tx_queues_current"] = nicTuningSecond(channels, "TX:")
		out["combined_queues_current"] = nicTuningSecond(channels, "Combined:")
	}
	return out
}

// nicTuningKey maps an ethtool -c field label to the flat map key used in
// NicTuning's output (snake_case, matching napi_defer_hard_irqs/
// gro_flush_timeout's style).
func nicTuningKey(label string) string {
	switch label {
	case "rx-usecs:":
		return "rx_usecs"
	case "tx-usecs:":
		return "tx_usecs"
	default:
		return strings.ToLower(strings.TrimSuffix(label, ":"))
	}
}

// nicTuningAdaptive parses ethtool -c's single combined line, e.g.
// "Adaptive RX: off  TX: n/a", returning (rx, tx) = ("off", "n/a"). Returns
// ("", "") if the line is missing or doesn't match the expected shape.
func nicTuningAdaptive(text string) (rx, tx string) {
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "Adaptive RX:") {
			continue
		}
		rest := strings.TrimSpace(strings.TrimPrefix(line, "Adaptive RX:"))
		parts := strings.SplitN(rest, "TX:", 2)
		rx = strings.TrimSpace(parts[0])
		if len(parts) == 2 {
			tx = strings.TrimSpace(parts[1])
		}
		return rx, tx
	}
	return "", ""
}

// nicTuningGrep returns the value on the first line starting with label
// (after trimming), or "" if not found - ethtool's plain-text output has no
// machine-readable form, so this is a simple field extractor rather than a
// full parser.
func nicTuningGrep(text, label string) string {
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, label) {
			return strings.TrimSpace(strings.TrimPrefix(line, label))
		}
	}
	return ""
}

// nicTuningSecond is like nicTuningGrep but returns the value from the SECOND
// matching line - ethtool -l repeats each field once under "Pre-set maximums"
// and once under "Current hardware settings"; only the second occurrence
// reflects what's actually configured.
func nicTuningSecond(text, label string) string {
	n := 0
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, label) {
			n++
			if n == 2 {
				return strings.TrimSpace(strings.TrimPrefix(line, label))
			}
		}
	}
	return ""
}

// FreeQueue frees AF_XDP queue 0: kill any transient sender/receiver + detach
// stale XDP. Safe to call on source/destination (NOT the replicator, whose XDP
// must stay) — the caller decides.
func (r *Runner) FreeQueue() error {
	// ONE sudo for the whole sequence: each sudo from this service context costs
	// ~125ms, so the previous five-invocation form dominated per-mode cleanup.
	// Polls for the killed processes to disappear rather than sleeping a flat
	// second; SIGKILL reaping is sub-10ms in the normal case.
	_, err := sh(`sudo bash -c '
		pkill -9 -x mcast_receive 2>/dev/null || true
		pkill -9 -x mcast_send 2>/dev/null || true
		for i in $(seq 1 100); do
			pgrep -x mcast_receive >/dev/null 2>&1 || pgrep -x mcast_send >/dev/null 2>&1 || break
			sleep 0.02
		done
		IFACE=$(ip -4 route show default | awk "{print \$5}" | head -1)
		ip link set "$IFACE" xdp off 2>/dev/null || true
		ip link set "$IFACE" xdpgeneric off 2>/dev/null || true
		xdp-loader unload "$IFACE" --all 2>/dev/null || true'`)
	return err
}

// SetFwdMode sets REPLICATOR_FWD_MODE (copy clears it) and restarts the replicator.
func (r *Runner) SetFwdMode(mode string) error {
	var script string
	if mode == "" || mode == "copy" {
		script = `sudo sed -i '/^REPLICATOR_FWD_MODE=/d' /etc/default/replicator`
	} else {
		script = fmt.Sprintf(`sudo sed -i '/^REPLICATOR_FWD_MODE=/d' /etc/default/replicator;
			echo REPLICATOR_FWD_MODE=%s | sudo tee -a /etc/default/replicator >/dev/null`, mode)
	}
	if _, err := sh(script + `; sudo systemctl restart replicator`); err != nil {
		return err
	}
	return r.waitReplicator(true, 30*time.Second)
}

// SetMode sets REPLICATOR_MODE (ucast|mcast|echo) and the fan-out FWD_MODE
// (copy clears it), then restarts the replicator so it re-attaches with the new
// program. Used to put the replicator node into mcast fan-out for a campaign.
func (r *Runner) SetMode(mode, fwd string) error {
	script := fmt.Sprintf(`sudo sed -i '/^REPLICATOR_MODE=/d' /etc/default/replicator;
		echo REPLICATOR_MODE=%s | sudo tee -a /etc/default/replicator >/dev/null;
		sudo sed -i '/^REPLICATOR_FWD_MODE=/d' /etc/default/replicator`, mode)
	if fwd != "" && fwd != "copy" {
		script += fmt.Sprintf(`; echo REPLICATOR_FWD_MODE=%s | sudo tee -a /etc/default/replicator >/dev/null`, fwd)
	}
	if _, err := sh(script + `; sudo systemctl restart replicator`); err != nil {
		return err
	}
	return r.waitReplicator(true, 30*time.Second)
}

// ReplicatorSvc stop|start|restarts replicator.service. Stopping it on a source
// or destination frees AF_XDP queue 0 for a transient sender/receiver.
func (r *Runner) ReplicatorSvc(action string) error {
	switch action {
	case "stop", "start", "restart":
	default:
		return fmt.Errorf("bad svc action %q", action)
	}
	if _, err := sh(fmt.Sprintf(`sudo systemctl %s replicator`, action)); err != nil {
		return err
	}
	return r.waitReplicator(action != "stop", 30*time.Second)
}

// JoinGroup registers this node as an mcast destination with the replicator.
func (r *Runner) JoinGroup(replicatorIP, group string) error {
	_, err := sh(fmt.Sprintf(`for i in $(seq 1 5); do sudo %s %s mcast %s && break; sleep 2; done`,
		r.bin("replicator_ctl"), replicatorIP, group))
	return err
}

// PurgeDests removes every destination currently registered on the LOCAL replicator.
//
// Why this exists: `rtt` self-registers with the remote replicator
// (CTRL_ADD_DESTINATION) and deregisters on exit — but a SIGKILLed / crashed rtt
// leaves a stale entry behind. In UNICAST mode the replicator fans out each echo
// to EVERY registered destination, so one stale entry makes every subsequent
// measurement against this node cost 2 sends per packet, two entries 3 sends, etc.
// That shows up as a CONSTANT ms-scale p50 shift with a normal spread. Purging
// before a campaign makes each run start from a known-clean registry.
func (r *Runner) PurgeDests() error {
	// `list` prints one "ip:port" per registered destination; feed each back to `remove`.
	script := fmt.Sprintf(`
		CTL=%s
		OUT=$(sudo $CTL 127.0.0.1 list 2>/dev/null) || exit 0
		echo "$OUT" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:[0-9]+' | sort -u | while IFS=: read -r ip port; do
			sudo $CTL 127.0.0.1 remove "$ip" "$port" >/dev/null 2>&1 || true
		done
		exit 0`, r.bin("replicator_ctl"))
	_, err := sh(script)
	return err
}

// ReplicatorMode returns the REPLICATOR_MODE value from /etc/default/replicator (ucast|mcast|echo).
func (r *Runner) ReplicatorMode() string {
	out, _ := sh(`grep -o 'REPLICATOR_MODE=[a-z]*' /etc/default/replicator 2>/dev/null | cut -d= -f2`)
	return strings.TrimSpace(out)
}

// ReplicatorActive returns the systemd active state of replicator.service.
func (r *Runner) ReplicatorActive() string {
	out, _ := sh(`systemctl is-active replicator 2>/dev/null`)
	return strings.TrimSpace(out)
}

// rttJSON matches the JSON the C++ rtt tool writes to /tmp/rtt_results.json,
// and (reused by RunMcastReceive) what mcast_receive writes to its -j path.
// hop1_us/hop2_us are mcast_receive-only: the source->replicator and
// replicator->destination legs of the one-way path, present only when the
// wire header carried a replicator timestamp (has_replicator_ts in
// mcast_receive.cpp) - nil otherwise, including for every rtt/ucast result.
type rttJSON struct {
	Messages int64   `json:"messages"`
	Lost     int64   `json:"lost"`
	LossPct  float64 `json:"loss_pct"`
	Skew     int64   `json:"clock_skew_samples"`
	Service  struct {
		Min, Mean, P50, P90, P95, P99, P999, Max int64
	} `json:"service_rtt_us"`
	Hop1 *hopPct `json:"hop1_us,omitempty"`
	Hop2 *hopPct `json:"hop2_us,omitempty"`
	// ElapsedS/AchievedPps: mcast_receive.cpp-only fields (rtt's own JSON has
	// its own achieved-rate reporting on stdout, not yet in its JSON file -
	// see toRTTMetrics below for that tool's separate path). Absent (zero
	// value) on any older result predating this field; toMetrics leaves
	// RequestedPps/RateShortfall unset in that case rather than computing a
	// misleading 0%.
	ElapsedS float64 `json:"elapsed_s,omitempty"`
	// ActiveS spans first->last packet received; achieved_pps is computed over
	// this window by mcast_receive, NOT over ElapsedS - see RxStats::first_rx_ns
	// there for why (sender startup dead time inside ElapsedS).
	ActiveS     float64 `json:"active_s,omitempty"`
	AchievedPps float64 `json:"achieved_pps,omitempty"`
}

// hopPct mirrors mcast_receive.cpp's hop1_us/hop2_us shape:
// {"p50":N,"p99":N,"p999":N} in microseconds - a smaller percentile set than
// service_rtt_us's, matching what the tool actually computes for the hop
// breakdown.
type hopPct struct {
	P50  int64 `json:"p50"`
	P99  int64 `json:"p99"`
	P999 int64 `json:"p999"`
}

func toMetrics(j rttJSON) proto.Metrics {
	m := proto.Metrics{
		ServiceRTT: proto.Pct{Min: j.Service.Min, Mean: j.Service.Mean, P50: j.Service.P50,
			P90: j.Service.P90, P95: j.Service.P95, P99: j.Service.P99, P999: j.Service.P999, Max: j.Service.Max},
		Messages: j.Messages, Lost: j.Lost, LossPct: j.LossPct, ClockSkewSamples: j.Skew,
		ElapsedS: j.ElapsedS, AchievedPps: j.AchievedPps,
	}
	if j.Hop1 != nil {
		m.Hop1 = &proto.HopPct{P50: j.Hop1.P50, P99: j.Hop1.P99, P999: j.Hop1.P999}
	}
	if j.Hop2 != nil {
		m.Hop2 = &proto.HopPct{P50: j.Hop2.P50, P99: j.Hop2.P99, P999: j.Hop2.P999}
	}
	return m
}

// applyRateExpectation fills RequestedPps/RateShortfall on m from
// requestedPps, the rate the orchestrator/CLI actually asked for (interval_us
// for mcast, rate for rtt) - the receiver tool itself has no way to know this,
// it only measures what arrived. 0 requestedPps (rate not specified, or
// interval_us==0/unbounded) leaves both fields at their zero value rather
// than computing a nonsensical ratio. The 90% threshold reflects that a
// saturated/under-rate run's percentiles reflect queueing, not per-packet
// cost, so flagging it surfaces that class of issue without needing manual
// log archaeology.
func applyRateExpectation(m *proto.Metrics, requestedPps float64) {
	if requestedPps <= 0 || m.AchievedPps <= 0 {
		return
	}
	m.RequestedPps = requestedPps
	m.RateShortfall = m.AchievedPps < 0.9*requestedPps
}

// RunRTT executes one rtt measurement and returns parsed metrics + the TX bind
// mode observed ("zero-copy" | "copy" | "" for kernel TX).
func (r *Runner) RunRTT(p proto.RTTParams) (proto.Metrics, string, error) {
	sendCPU, recvCPU := p.SendCPU, p.RecvCPU
	if sendCPU < 0 || recvCPU < 0 {
		s, rc := derivePins()
		if sendCPU < 0 {
			sendCPU = s
		}
		if recvCPU < 0 {
			recvCPU = rc
		}
	}
	flags := ""
	if p.XdpTx {
		q := p.XdpTxQueue
		if q == 0 {
			q = 1
		}
		flags += fmt.Sprintf(" --xdp-tx=%d --iface %s", q, iface())
	}
	if p.XdpRx {
		flags += " --xdp-rx"
	}

	// Readiness confirm: one control round-trip to the target so a measurement is
	// never launched against a replicator that cannot echo. The orchestrator
	// already waits for the target's restore to finish, so this is a backstop.
	// replicator_ctl carries its own multi-second receive timeout, so retrying
	// here would cost seconds per miss.
	probe := fmt.Sprintf(`sudo %s %s list >/dev/null 2>&1`, r.bin("replicator_ctl"), p.TargetIP)
	if _, err := sh(probe); err != nil {
		return proto.Metrics{}, "", fmt.Errorf("target %s replicator did not answer its control port", p.TargetIP)
	}

	_ = os.Remove("/tmp/rtt_results.json")
	cmd := fmt.Sprintf(`%s %s %d %s %d %d %d %d %d %d%s >/tmp/rtt_out.txt 2>&1`,
		r.bin("rtt"), p.TargetIP, p.DataPort, p.ListenIP, p.ListenPort,
		p.Count, p.Rate, p.Warmup, sendCPU, recvCPU, flags)

	// ── Retry: if rtt exits non-zero, sleep 1s and retry once. Covers
	// transient subscribe failures (replicator accepted TCP but hasn't
	// finished initializing its AF_XDP ring yet — <1s window).
	var rttErr error
	for attempt := 0; attempt < 2; attempt++ {
		_ = os.Remove("/tmp/rtt_results.json")
		if _, err := sh(cmd); err != nil {
			rttErr = fmt.Errorf("rtt exec: %w", err)
			if attempt == 0 {
				time.Sleep(1 * time.Second)
				continue
			}
		} else {
			rttErr = nil
			break
		}
	}
	if rttErr != nil {
		// Attach stdout for diagnostics.
		if out, e := os.ReadFile("/tmp/rtt_out.txt"); e == nil && len(out) > 0 {
			rttErr = fmt.Errorf("%w - %s", rttErr, strings.TrimSpace(string(out)))
		}
		return proto.Metrics{}, "", rttErr
	}
	txMode := ""
	if out, e := os.ReadFile("/tmp/rtt_out.txt"); e == nil {
		s := string(out)
		if strings.Contains(s, "(zero-copy)") {
			txMode = "zero-copy"
		} else if strings.Contains(s, "COPY/SKB") {
			txMode = "copy"
		}
	}
	b, err := os.ReadFile("/tmp/rtt_results.json")
	if err != nil {
		return proto.Metrics{}, txMode, fmt.Errorf("rtt produced no results: %w", err)
	}
	var j rttJSON
	if err := json.Unmarshal(b, &j); err != nil {
		return proto.Metrics{}, txMode, fmt.Errorf("rtt json: %w", err)
	}
	// NO-SAMPLES GATE — rtt omits service_rtt_us entirely when every sample was
	// discarded, and an all-zero struct unmarshals silently into p50=0. A 0 µs
	// RTT is physically impossible, so publishing it would put a plausible-looking
	// number in the report. The common trigger is --xdp-rx with no XDP program
	// attached on the client: the stamp slot stays zero, every sample fails the
	// 0 < rtt < 100ms sanity filter, and the run still reports "Lost: 0".
	if j.Service.P50 <= 0 || j.Service.Max <= 0 {
		hint := ""
		if p.XdpRx {
			hint = " (--xdp-rx requires the XDP program attached on this interface " +
				"to stamp the RX time; verify with `ip -d link show`)"
		}
		return proto.Metrics{}, txMode, fmt.Errorf(
			"rtt produced no valid latency samples: p50=%d max=%d over %d messages%s",
			j.Service.P50, j.Service.Max, j.Messages, hint)
	}
	// LOSS GATE — fail the measurement outright rather than publishing biased
	// percentiles. rtt derives its percentiles only from datagrams that actually
	// returned, so a run that loses X% reports the latency distribution of the
	// (100-X)% that survived. That subset is not a random sample, so its p50 is
	// not comparable to a clean run's p50 and silently understates or overstates
	// the true figure. Returning an error here means the orchestrator marks the
	// pair failed and the collector stores nothing — a visible gap instead of a
	// plausible-looking wrong number.
	if p.MaxLossPct > 0 && j.LossPct > p.MaxLossPct {
		return proto.Metrics{}, txMode, fmt.Errorf(
			"loss gate: %.2f%% loss exceeds max %.2f%% (%d/%d datagrams lost) — "+
				"percentiles would be computed over a survivorship-biased subset, measurement rejected",
			j.LossPct, p.MaxLossPct, j.Lost, j.Messages)
	}
	m := toMetrics(j)
	applyRateExpectation(&m, float64(p.Rate))
	return m, txMode, nil
}

// derivePins returns the rtt send/recv CPU cores as the top two members of the
// intersection of isolcpus and the online CPU set, highest first (send=highest,
// recv=second). Using online ∩ isolated guarantees the returned cores exist on
// every instance type regardless of how nosmt/isolcpus interact:, highest-first (send = highest, recv = second). This
// adapts to instance size instead of assuming a fixed lo+2/lo+3 offset:
//   - >=4xlarge, isolcpus=1-4 all online  -> send=4, recv=3 (as intended)
//   - 4-core 2xlarge, nosmt takes the upper SMT siblings offline so isolcpus=1-4
//     yields only 1-3 online -> send=3, recv=2
//
// Crucially it NEVER returns an offline CPU: pinning a thread to an offline core
// silently fails and the thread falls back to the contended housekeeping CPU 0,
// which was inflating/destabilising the AF_XDP TX path.
func derivePins() (send, recv int) {
	iso := onlineIsolatedCPUs()
	if n := len(iso); n >= 2 {
		return iso[n-1], iso[n-2]
	} else if n == 1 {
		return iso[0], iso[0]
	}
	return 3, 2 // fallback when no isolation info is available
}

// onlineIsolatedCPUs returns the sorted CPUs present in BOTH isolcpus and the
// kernel's online set.
func onlineIsolatedCPUs() []int {
	online := map[int]bool{}
	for _, c := range parseCPUList(readSysCPU("/sys/devices/system/cpu/online")) {
		online[c] = true
	}
	var out []int
	for _, c := range parseCPUList(readIsolcpus()) {
		if online[c] {
			out = append(out, c)
		}
	}
	sort.Ints(out)
	return out
}

func readSysCPU(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

// parseCPUList parses a Linux CPU list like "1-4,6" into [1 2 3 4 6].
func parseCPUList(s string) []int {
	var out []int
	for _, part := range strings.Split(strings.TrimSpace(s), ",") {
		if part == "" {
			continue
		}
		if i := strings.IndexByte(part, '-'); i >= 0 {
			lo, e1 := strconv.Atoi(part[:i])
			hi, e2 := strconv.Atoi(part[i+1:])
			if e1 == nil && e2 == nil {
				for c := lo; c <= hi; c++ {
					out = append(out, c)
				}
			}
		} else if v, err := strconv.Atoi(part); err == nil {
			out = append(out, v)
		}
	}
	return out
}

// RunMcastReceive starts mcast_receive in the foreground (bounded by timeout),
// reads its JSON result. Intended to be launched concurrently with a sender.
func (r *Runner) RunMcastReceive(p proto.McastParams) (proto.Metrics, error) {
	_, recv := derivePins()
	_ = os.Remove("/tmp/mcast_results.json")
	// Clear the previous run's log so McastRxReady cannot read a stale
	// "listening" line from an earlier mode as readiness for this one.
	_ = os.Remove(mcastRxLog)
	flags := ""
	if p.RxQueue != 0 {
		flags += fmt.Sprintf(" -q %d", p.RxQueue)
	}
	if p.Variation == "kernel" {
		flags += " -k"
	}
	cmd := fmt.Sprintf(`sudo timeout %d taskset -c %d %s -I %s -g %s -p %d -c %d -t %d%s -j /tmp/mcast_results.json >/tmp/mcast_receive.log 2>&1`,
		p.TimeoutSec+5, recv, r.bin("mcast_receive"), iface(), p.Group, p.DataPort, p.Count, p.TimeoutSec, flags)
	if _, err := sh(cmd); err != nil {
		// timeout/exit is expected; fall through to read JSON
		_ = err
	}
	b, err := os.ReadFile("/tmp/mcast_results.json")
	if err != nil {
		return proto.Metrics{}, fmt.Errorf("mcast_receive produced no results: %w", err)
	}
	var j rttJSON
	if err := json.Unmarshal(b, &j); err != nil {
		return proto.Metrics{}, err
	}
	if reason := mcastAbnormalTail(j); reason != "" {
		// Returning an error here (rather than the metrics) routes this
		// result into RunMcastMatrix's existing attempt<=2 retry loop
		// (orchestrator.go) for free - that loop already re-runs the full
		// send+receive cycle on !res.OK, which is what a stall like this
		// needs: re-synchronizing both sides, not just re-reading a socket
		// that may still be in the stuck state. See mcastAbnormalTail's
		// SCHED_FIFO busy-poll comment below for what actually causes this.
		return proto.Metrics{}, fmt.Errorf("mcast_receive: %s", reason)
	}
	// CLOCK-SKEW GATE - reject rather than publish a skewed one-way split.
	//
	// mcast_receive already counts samples where rx_ns < ts_ns (negative total)
	// and reports them as clock_skew_samples, but nothing acted on the number:
	// the run was stored looking identical to a clean one. A one-way measurement
	// is only meaningful if both clocks agree, so any confirmed skew sample
	// invalidates the percentiles the same way loss does (see the LOSS GATE in
	// RunRTT above for the same argument applied to survivorship bias).
	//
	// Note the limits of this detector, so it is not mistaken for full coverage:
	// it fires ONLY when the destination clock is BEHIND by more than the transit
	// time. A destination clock running AHEAD inflates hop2 while keeping it
	// positive and is NOT caught, and the detection threshold is the transit time
	// itself (~16us same-AZ, ~10ms cross-region). It is a floor, not a guarantee.
	if j.Skew > 0 {
		return proto.Metrics{}, fmt.Errorf(
			"clock-skew gate: %d/%d samples had rx<tx (destination clock behind source) - "+
				"one-way latency is invalid, measurement rejected; re-sync chrony "+
				"(chronyc makestep) on both nodes and confirm `chronyc tracking` System time",
			j.Skew, j.Messages)
	}
	m := toMetrics(j)
	// p.IntervalUs is mcast_send's inter-packet gap in µs; 1e6/IntervalUs is
	// the rate that setting implies. IntervalUs==0 means mcast_send ran
	// unbounded (no pacing), so there is no meaningful "requested" rate to
	// compare against - applyRateExpectation leaves the fields unset for that
	// case via its own requestedPps<=0 guard.
	requestedPps := 0.0
	if p.IntervalUs > 0 {
		requestedPps = 1e6 / float64(p.IntervalUs)
	}
	applyRateExpectation(&m, requestedPps)
	return m, nil
}

// mcastAbnormalTail flags a mcast_receive result whose tail latency is
// orders of magnitude above its own p50 as unusable, rather than a real
// measurement. This is not queueing or loss (mcast_send's own saturation
// point produces uniformly high percentiles, not a p50/p99 gap like this) -
// it is a rare SCHED_FIFO busy-poll stall where poll()'s sk_busy_loop() spins
// inside the kernel for up to ~1-2s on an isolated, nohz_full, non-preemptible
// RT-priority core. All packets still arrive (often 0% loss)
// just very late, so loss_pct alone does not catch it - only the p50-vs-tail
// shape does. Returns "" when the result looks like a normal measurement.
func mcastAbnormalTail(j rttJSON) string {
	if j.Messages == 0 {
		return "" // nothing received; RunMcastReceive's caller already errors on TimeoutSec expiry
	}
	// 1ms is far above any real one-way mcast latency on this fleet (tens of
	// us even at worst-case cross-region), so it cleanly separates a real tail
	// from a stall without risking false positives on a legitimately slow run.
	const stallFloorUs = 1000
	if j.Service.Max < stallFloorUs {
		return ""
	}
	// Require the max to be a large multiple of p50, not just numerically
	// above the floor - a genuinely loaded/saturated run can have a p50 in
	// the hundreds of us with a proportionally scaled max; that is real
	// queueing, not this stall, and should surface as a normal (bad) result
	// rather than be silently retried away.
	p50 := j.Service.P50
	if p50 <= 0 {
		p50 = 1
	}
	if j.Service.Max >= p50*50 {
		return fmt.Sprintf("pathological tail: p50=%dus but max=%dus (>=50x) - "+
			"SCHED_FIFO busy-poll stall, not a real measurement", p50, j.Service.Max)
	}
	return ""
}

// RunMcastSend runs an mcast_send burst (m2u to the replicator).
func (r *Runner) RunMcastSend(p proto.McastParams) error {
	_, recv := derivePins()
	send := recv // sender uses an isolated core too
	flags := ""
	if p.Size != 0 {
		flags += fmt.Sprintf(" -s %d", p.Size)
	}
	if p.TxQueue != 0 {
		flags += fmt.Sprintf(" -q %d", p.TxQueue)
	}
	if p.Variation == "kernel" {
		flags += " -k"
	}
	cmd := fmt.Sprintf(`sudo taskset -c %d %s -I %s -D %s -g %s -p %d -c %d -i %d%s`,
		send, r.bin("mcast_send"), iface(), p.ReplicatorIP, p.Group, p.DataPort, p.Count, p.IntervalUs, flags)
	_, err := sh(cmd)
	return err
}

// nowUnix is a tiny helper.
func nowUnix() int64 { return time.Now().Unix() }

// mcastRxLog is where the agent captures mcast_receive's stdout; its "AF_XDP
// listening" line is the receiver's readiness signal.
const mcastRxLog = "/tmp/mcast_receive.log"

// McastRxReady reports whether a local mcast_receive is running AND has attached
// its XDP program and bound its socket. Both conditions are required: the process
// alone is not ready yet, and the log alone could be stale from a previous mode.
// This replaces a blind settle sleep before the source starts sending.
func (r *Runner) McastRxReady() bool {
	if _, err := sh(`pgrep -x mcast_receive >/dev/null 2>&1`); err != nil {
		return false
	}
	// "AF_XDP listening" (AF_XDP fwd modes) or "kernel socket listening"
	// (-k / REPLICATOR_FWD_MODE=kernel, see run_kernel_receive in
	// mcast_receive.cpp) — either readiness line satisfies this check.
	_, err := sh(fmt.Sprintf(`grep -Eq "AF_XDP listening|kernel socket listening" %s 2>/dev/null`, mcastRxLog))
	return err == nil
}

// ─────────────────────────────────────────────────────────────────────────────
// Wander sampler. StartWander/StopWander bound
// phcsample's lifetime to exactly one (replicator, mode) run's settle+run
// window — the sampler is a DIRECT CHILD of this process, never detached,
// never a systemd unit, and is guaranteed to be reaped by StopWander or by
// its own context deadline, whichever comes first. This is deliberately NOT
// the systemd-run/setsid pattern used by the ad-hoc lab tooling
// let an SSH call return while sampling continues unsupervised, which is
// precisely the "persistent background daemon" shape this feature must not
// have. Here the sampler's lifetime is the campaign command's lifetime.
// ─────────────────────────────────────────────────────────────────────────────

// phcCounterKeys are the ENA PHC health counters G4 (wander-band-design.md
// §5) reads from `ethtool -S <IF> | grep phc`, bracketing the sampling
// window at start and end. Vendor guidance (precision-opus5-high-design.md
// V-21): PHC errors must stay below 1% of requests, and phc_err_ts/
// phc_err_eb/phc_skp must not move at all during a clean window - any
// movement means the device hit its 125/s get-time cap or the block period
// it triggers, which corrupts the wander evidence for the samples taken
// during that window.
var phcCounterKeys = []string{"phc_cnt", "phc_exp", "phc_skp", "phc_err_dv", "phc_err_ts", "phc_err_eb"}

// PhcCounters reads the ENA PHC health counters from `ethtool -S <IF>` on
// this node's default interface, for G4's window-start/window-end bracket.
// Missing keys (older driver, or ethtool -S produced no phc_* lines at all)
// are simply absent from the returned map - the caller's gate logic treats
// an absent counter as "cannot verify", not as a passing zero, so an older
// fleet node never gets a false pass on a gate it cannot actually answer.
func (r *Runner) PhcCounters() map[string]int64 {
	nic := iface()
	if nic == "" {
		return nil
	}
	out, err := sh(fmt.Sprintf(`ethtool -S %s 2>/dev/null | grep -E 'phc_(cnt|exp|skp|err_dv|err_ts|err_eb):'`, nic))
	if err != nil {
		return nil
	}
	return parsePhcCounters(out)
}

// parsePhcCounters is PhcCounters' pure parsing half, split out so the
// key-extraction logic is testable without a real ethtool/NIC.
func parsePhcCounters(out string) map[string]int64 {
	counters := map[string]int64{}
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimSpace(line)
		for _, key := range phcCounterKeys {
			prefix := key + ":"
			if strings.HasPrefix(line, prefix) {
				v, perr := strconv.ParseInt(strings.TrimSpace(strings.TrimPrefix(line, prefix)), 10, 64)
				if perr == nil {
					counters[key] = v
				}
			}
		}
	}
	if len(counters) == 0 {
		return nil
	}
	return counters
}

// RefclockSelected reports whether chronyc sources shows exactly one source
// unambiguously selected (the '*' in the S state column - see
// parseRefclockSelected for the exact column position), AND no more than one
// PHC refclock line present, regardless of whether the selected source is
// the local PHC or an NTP peer.
//
// G5 (wander-band-design.md §5) originally required the selected source to
// be specifically the PHC, on the assumption that PHC-selected is this
// fleet's normal state and NTP-fallback is the anomaly to catch. That
// assumption does not hold universally: configure_mcast.yaml deliberately
// DISABLES the PHC refclock in favor of AWS Time Sync NTP for one-way mcast
// latency accuracy (two hosts' independent PHCs disagree by ~100-200us,
// while NTP disciplines every node to the same shared reference, <10us
// inter-instance - see that playbook's own "ROOT-CAUSE FIX" comment). A
// campaign run through that playbook has NTP as the intended, correct
// state, and the original PHC-only check rejected every such run for a
// config choice that was working exactly as designed.
//
// G5's real job - per its own stated rationale, "node silently on NTP
// fallback... a different measurement entirely" - is to catch clock
// provenance the wander evidence's caller did NOT expect, not to prefer one
// specific source type over another. Accepting either PHC or NTP as the
// selected source, while still rejecting more than one PHC line (the
// duplicate-refclock config defect's actual signature - see
// parseRefclockSelected's doc comment), keeps catching the real failure
// modes without flagging a healthy, deliberately-configured NTP-preferred
// node as if its clock provenance were suspect.
func (r *Runner) RefclockSelected() (bool, error) {
	out, err := sh(`chronyc sources 2>/dev/null`)
	if err != nil {
		return false, fmt.Errorf("chronyc sources: %w", err)
	}
	return parseRefclockSelected(out), nil
}

// parseRefclockSelected is RefclockSelected's pure parsing half.
//
// chronyc sources' first two characters are the M (mode: ^=server, ==peer,
// #=local reference clock, i.e. what a PHC refclock reports as) and S
// (state) columns. The SELECTED marker is '*' in the STATE column - the
// SECOND character of the line, not the first. An earlier version of this
// function checked HasPrefix(trimmed, "*") and never matched a real PHC
// line, since '#' (mode) always occupies position 0 for a refclock source;
// caught by TestParseRefclockSelected using a real "#* PHC0 ..." fixture,
// which failed against the leading-character check and forced this fix.
//
// Requires: exactly one source SELECTED ('*'), and no more than one PHC
// refclock line present regardless of selection state. The second half is
// what a plain "exactly one '*'" count would miss: the live duplicate-parse
// bug's actual chronyc output is "#+ PHC0 ... / #* PHC1 ..." - PHC0 as an
// unselected backup, PHC1 selected - which already has exactly one '*' and
// would pass a selection-count-only check while the underlying config is
// still broken (one refclock directive registered twice). Counting PHC
// lines specifically (not source lines in general) catches that shape
// without going back to requiring PHC over NTP: a fleet with the PHC
// refclock disabled (configure_mcast.yaml's deliberate NTP-preferred state)
// has zero PHC lines, which trivially satisfies "no more than one," so a
// healthy NTP-only node still passes.
func parseRefclockSelected(out string) bool {
	selectedLines := 0
	phcLines := 0
	for _, line := range strings.Split(out, "\n") {
		trimmed := strings.TrimSpace(line)
		if len(trimmed) < 2 {
			continue
		}
		mode := trimmed[0]
		if mode != '^' && mode != '=' && mode != '#' {
			continue // not a source line (e.g. the header/separator rows)
		}
		if trimmed[1] == '*' {
			selectedLines++
		}
		if strings.Contains(trimmed, "PHC") {
			phcLines++
		}
	}
	return selectedLines == 1 && phcLines <= 1
}

// realWanderCmd builds phcsample's real exec.Cmd for a given duration. "" is
// never returned for bin/args - a failure to resolve the PTP device or PCI
// slot is a hard error, so StartWander can fail fast with a clear message
// instead of letting exec.Command fail opaquely on a missing/empty argument.
// Parameters match wander-band-design.md §3: /dev/ptp_ena preferred,
// /dev/ptp0 fallback; 10 Hz, 5 samples/ioctl, phc_error_bound sysfs path
// derived from the PCI slot of the default interface.
func (r *Runner) realWanderCmd(ctx context.Context, seconds int) (*exec.Cmd, error) {
	ptpDev := "/dev/ptp_ena"
	if _, statErr := os.Stat(ptpDev); statErr != nil {
		ptpDev = "/dev/ptp0"
		if _, statErr := os.Stat(ptpDev); statErr != nil {
			return nil, fmt.Errorf("no PTP device found (/dev/ptp_ena or /dev/ptp0)")
		}
	}
	nic := iface()
	if nic == "" {
		return nil, fmt.Errorf("could not determine default interface for phc_error_bound path")
	}
	pciOut, err := sh(fmt.Sprintf(`cat /sys/class/net/%s/device/uevent 2>/dev/null | grep -o 'PCI_SLOT_NAME=.*' | cut -d= -f2`, nic))
	if err != nil || strings.TrimSpace(pciOut) == "" {
		return nil, fmt.Errorf("could not resolve PCI slot for %s: %w", nic, err)
	}
	pci := strings.TrimSpace(pciOut)
	ebPath := fmt.Sprintf("/sys/bus/pci/devices/%s/phc_error_bound", pci)
	// Invoked directly, NOT wrapped in sudo: this agent process already runs
	// as root (see afxdp-agent.service's User=root), and wrapping in sudo
	// anyway inserts an extra process between exec.CommandContext and
	// phcsample itself. sudo does not forward the signals it receives to its
	// child by default, so ctx's cancellation (StopWander, or the context
	// deadline) kills the sudo wrapper while leaving phcsample running
	// underneath it - an orphaned process still holding the PTP device open,
	// exactly the "persistent background process" outcome this feature's
	// whole design exists to prevent. Confirmed live on real EC2 hardware:
	// `ps -ef` after a StopWander call showed the sudo PID gone and the
	// phcsample PID still running as its own orphan. Direct invocation means
	// exec.CommandContext's kill targets phcsample itself.
	return exec.CommandContext(ctx, r.bin("phcsample"), ptpDev, ebPath, "10", strconv.Itoa(seconds), "5"), nil
}

// StartWander launches phcsample as a direct child bounded by a context with
// a deadline of seconds+margin (wander-band-design.md §3's "campaign duration
// + 30s margin"), and returns immediately — it does NOT block for the
// sampler's lifetime. Concurrent starts are rejected: only one sampler
// per node at a time, matching the one-PTP-device-per-node reality
// (a second phcsample would just collide on the same ioctl, per §3's EBUSY
// handling note).
func (r *Runner) StartWander(seconds int) error {
	if seconds <= 0 {
		return fmt.Errorf("wander sampler: seconds must be > 0, got %d", seconds)
	}
	r.wanderMu.Lock()
	defer r.wanderMu.Unlock()
	if r.wanderCancel != nil {
		return fmt.Errorf("wander sampler already running on this node")
	}

	const margin = 30 * time.Second
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(seconds)*time.Second+margin)
	cmd, err := r.wanderCmd(ctx, seconds)
	if err != nil {
		cancel()
		return fmt.Errorf("wander sampler: %w", err)
	}
	r.wanderOut.Reset()
	r.wanderErr = nil
	cmd.Stdout = &r.wanderOut
	cmd.Stderr = &r.wanderOut

	if err := cmd.Start(); err != nil {
		cancel()
		return fmt.Errorf("wander sampler: start: %w", err)
	}

	done := make(chan struct{})
	r.wanderCancel = cancel
	r.wanderDone = done
	// Wait() in its own goroutine so Start does not block. exec.CommandContext
	// guarantees the process is killed (SIGKILL) if ctx is cancelled or its
	// deadline passes BEFORE Wait returns — this is the property StopWander's
	// early-cancel test exercises: the child never survives past ctx, whether
	// StopWander cancels it explicitly or the deadline fires unattended.
	go func() {
		waitErr := cmd.Wait()
		r.wanderMu.Lock()
		r.wanderErr = waitErr
		r.wanderMu.Unlock()
		close(done)
	}()
	return nil
}

// StopWander cancels the running sampler (if any), waits for the child to
// exit, and returns its collected stdout (phcsample's CSV rows) for the
// caller to persist/reduce server-side. Safe to call when no sampler is
// running (returns "", nil) — CmdWanderStop's handler does not need to know
// whether CmdWanderStart actually succeeded on this node.
//
// A non-zero exit from phcsample (e.g. it hit the 20-consecutive-EBUSY-
// failures give-up condition from wander-band-design.md §3) is NOT treated
// as a StopWander error: partial CSV output is still useful data, and the
// server-side reduction's own gates (G1 sample_count, G2 sentinel_count)
// are what should reject an unusable run — not a transport-level error here
// that would discard the CSV before it's even inspected.
func (r *Runner) StopWander() (string, error) {
	r.wanderMu.Lock()
	cancel := r.wanderCancel
	done := r.wanderDone
	r.wanderMu.Unlock()
	if cancel == nil {
		return "", nil // nothing running — not an error, see doc comment
	}

	cancel()
	<-done

	r.wanderMu.Lock()
	out := r.wanderOut.String()
	r.wanderCancel = nil
	r.wanderDone = nil
	r.wanderMu.Unlock()
	return out, nil
}
