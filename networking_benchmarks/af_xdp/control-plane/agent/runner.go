package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"time"

	"afxdp-cp/proto"
)

// Runner executes the C++ measurement tools and owns the node-local resource
// lifecycle. The reproducibility fixes learned the hard way (free the AF_XDP
// queue before/after a run; makestep the clock; report skew) live HERE, so the
// backend never micromanages them over the wire — it just issues intents.
type Runner struct {
	binDir string // e.g. /opt/af-xdp
}

// NewRunner creates a Runner that drives the C++ measurement tools in binDir.
func NewRunner(binDir string) *Runner { return &Runner{binDir: binDir} }

func (r *Runner) bin(name string) string { return r.binDir + "/" + name }

// sh runs a bash snippet, returning combined output.
func sh(script string) (string, error) {
	out, err := exec.Command("bash", "-lc", script).CombinedOutput()
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

// clockOffsetUs reads chronyc's last offset (absolute), in microseconds.
func clockOffsetUs() float64 {
	out, err := sh(`chronyc tracking 2>/dev/null | awk -F': *' '/Last offset/{print $2}' | awk '{print $1}'`)
	if err != nil {
		return -1
	}
	v, err := strconv.ParseFloat(strings.TrimSpace(out), 64)
	if err != nil {
		return -1
	}
	return math.Abs(v) * 1e6
}

// ClockSync forces convergence (makestep + burst) and returns the achieved offset.
func (r *Runner) ClockSync() (float64, error) {
	if _, err := sh(`chronyc makestep >/dev/null 2>&1 || true; chronyc burst 3/4 >/dev/null 2>&1 || true;
		for i in $(seq 1 10); do chronyc tracking 2>/dev/null | grep -q "Leap status *: *Normal" && break; sleep 1; done`); err != nil {
		return -1, err
	}
	return clockOffsetUs(), nil
}

// FreeQueue frees AF_XDP queue 0: kill any transient sender/receiver + detach
// stale XDP. Safe to call on source/destination (NOT the replicator, whose XDP
// must stay) — the caller decides.
func (r *Runner) FreeQueue() error {
	_, err := sh(`sudo pkill -9 -x mcast_receive 2>/dev/null || true;
		sudo pkill -9 -x mcast_send 2>/dev/null || true; sleep 1;
		IFACE=$(ip -4 route show default | awk '{print $5}' | head -1);
		sudo ip link set "$IFACE" xdp off 2>/dev/null || true;
		sudo ip link set "$IFACE" xdpgeneric off 2>/dev/null || true;
		sudo xdp-loader unload "$IFACE" --all 2>/dev/null || true`)
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
	_, err := sh(script + `; sudo systemctl restart replicator; sleep 6`)
	return err
}

// SetMode sets REPLICATOR_MODE (ucast|mcast|kernel) and the fan-out FWD_MODE
// (copy clears it), then restarts the replicator so it re-attaches with the new
// program. Used to put the replicator node into mcast fan-out for a campaign.
func (r *Runner) SetMode(mode, fwd string) error {
	script := fmt.Sprintf(`sudo sed -i '/^REPLICATOR_MODE=/d' /etc/default/replicator;
		echo REPLICATOR_MODE=%s | sudo tee -a /etc/default/replicator >/dev/null;
		sudo sed -i '/^REPLICATOR_FWD_MODE=/d' /etc/default/replicator`, mode)
	if fwd != "" && fwd != "copy" {
		script += fmt.Sprintf(`; echo REPLICATOR_FWD_MODE=%s | sudo tee -a /etc/default/replicator >/dev/null`, fwd)
	}
	_, err := sh(script + `; sudo systemctl restart replicator; sleep 6`)
	return err
}

// ReplicatorSvc stop|start|restarts replicator.service. Stopping it on a source
// or destination frees AF_XDP queue 0 for a transient sender/receiver.
func (r *Runner) ReplicatorSvc(action string) error {
	switch action {
	case "stop", "start", "restart":
	default:
		return fmt.Errorf("bad svc action %q", action)
	}
	_, err := sh(fmt.Sprintf(`sudo systemctl %s replicator; sleep 3`, action))
	return err
}

// JoinGroup registers this node as an mcast destination with the replicator.
func (r *Runner) JoinGroup(replicatorIP, group string) error {
	_, err := sh(fmt.Sprintf(`for i in $(seq 1 5); do sudo %s %s mcast %s && break; sleep 2; done`,
		r.bin("replicator_ctl"), replicatorIP, group))
	return err
}

// ReplicatorMode returns the REPLICATOR_MODE value from /etc/default/replicator (ucast|mcast|kernel).
func (r *Runner) ReplicatorMode() string {
	out, _ := sh(`grep -o 'REPLICATOR_MODE=[a-z]*' /etc/default/replicator 2>/dev/null | cut -d= -f2`)
	return strings.TrimSpace(out)
}

// ReplicatorActive returns the systemd active state of replicator.service.
func (r *Runner) ReplicatorActive() string {
	out, _ := sh(`systemctl is-active replicator 2>/dev/null`)
	return strings.TrimSpace(out)
}

// rttJSON matches the JSON the C++ rtt tool writes to /tmp/rtt_results.json.
type rttJSON struct {
	Messages int64   `json:"messages"`
	Lost     int64   `json:"lost"`
	LossPct  float64 `json:"loss_pct"`
	Skew     int64   `json:"clock_skew_samples"`
	Service  struct {
		Min, Mean, P50, P90, P95, P99, P999, Max int64
	} `json:"service_rtt_us"`
}

func toMetrics(j rttJSON) proto.Metrics {
	return proto.Metrics{
		ServiceRTT: proto.Pct{Min: j.Service.Min, Mean: j.Service.Mean, P50: j.Service.P50,
			P90: j.Service.P90, P95: j.Service.P95, P99: j.Service.P99, P999: j.Service.P999, Max: j.Service.Max},
		Messages: j.Messages, Lost: j.Lost, LossPct: j.LossPct, ClockSkewSamples: j.Skew,
	}
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
	_ = os.Remove("/tmp/rtt_results.json")
	// Capture stdout so we can report the actual TX datapath (zero-copy vs the
	// copy/SKB fallback) per run instead of inferring it from latency.
	cmd := fmt.Sprintf(`%s %s %d %s %d %d %d %d %d %d%s >/tmp/rtt_out.txt 2>&1`,
		r.bin("rtt"), p.TargetIP, p.DataPort, p.ListenIP, p.ListenPort,
		p.Count, p.Rate, p.Warmup, sendCPU, recvCPU, flags)
	if _, err := sh(cmd); err != nil {
		return proto.Metrics{}, "", fmt.Errorf("rtt exec: %w", err)
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
	return toMetrics(j), txMode, nil
}

// derivePins returns the rtt send/recv CPU cores as the top two members of the
// intersection of isolcpus and the online CPU set, highest first (send=highest,
// recv=second). Using online ∩ isolated guarantees the returned cores exist on
// every instance type regardless of how nosmt/isolcpus interact:, highest-first (send = highest, recv = second). This
// adapts to instance size instead of assuming a fixed lo+2/lo+3 offset:
//   - >=4xlarge, isolcpus=1-4 all online  -> send=4, recv=3 (as intended)
//   - 4-core 2xlarge, nosmt takes the upper SMT siblings offline so isolcpus=1-4
//     yields only 1-3 online -> send=3, recv=2
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
	cmd := fmt.Sprintf(`sudo timeout %d taskset -c %d %s -I %s -g %s -p %d -c %d -t %d -j /tmp/mcast_results.json >/tmp/mcast_receive.log 2>&1`,
		p.TimeoutSec+5, recv, r.bin("mcast_receive"), iface(), p.Group, p.DataPort, p.Count, p.TimeoutSec)
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
	return toMetrics(j), nil
}

// RunMcastSend runs an mcast_send burst (m2u to the replicator).
func (r *Runner) RunMcastSend(p proto.McastParams) error {
	_, recv := derivePins()
	send := recv // sender uses an isolated core too
	cmd := fmt.Sprintf(`sudo taskset -c %d %s -I %s -D %s -g %s -p %d -c %d -i %d`,
		send, r.bin("mcast_send"), iface(), p.ReplicatorIP, p.Group, p.DataPort, p.Count, p.IntervalUs)
	_, err := sh(cmd)
	return err
}

// nowUnix is a tiny helper.
func nowUnix() int64 { return time.Now().Unix() }
