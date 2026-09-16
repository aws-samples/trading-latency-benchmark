package main

import (
	"fmt"
	"os"
	"strings"
	"time"

	"afxdp-cp/proto"
)

// Idempotent host-state convergence.
//
// Each measurement role needs a specific local configuration, and the wrong one
// silently corrupts results rather than failing loudly. The worst case found in
// practice: a node acting as the rtt CLIENT while its own replicator still holds
// an AF_XDP zero-copy socket on the RX queue the echoes return on. Returning
// echoes don't match the replicator's config_map, so the XDP program XDP_PASSes
// them, which in ZC mode makes the driver copy each frame out of the UMEM and
// recycle it through the fill queue the replicator's poll loop owns. When that
// loop isn't draining promptly the NIC drops arriving echoes — 0-95% loss,
// erratic, in contiguous multi-hundred-millisecond windows, with no drop counter
// attributing it to our flow. Percentiles are computed only over datagrams that
// returned, so the result looked plausible while being survivorship-biased.
//
// EFFICIENCY. Converging is deliberately cheap when nothing must change: one
// combined state read (a single shell round-trip), then only the transitions that
// actually differ. Re-applying the profile a node is already in performs no
// systemctl work and no sleep. Where a restart IS required we poll for readiness
// instead of sleeping a fixed worst-case interval.

const standaloneMarker = "/run/afxdp-standalone-xdp"

// replicatorEnvFile holds REPLICATOR_MODE / REPLICATOR_FWD_MODE, read by
// start-replicator.sh at launch.
const replicatorEnvFile = "/etc/default/replicator"

// hostState is the observed local configuration.
type hostState struct {
	svcActive   bool   // replicator.service is active
	mode        string // REPLICATOR_MODE (ucast|mcast|echo)
	fwd         string // REPLICATOR_FWD_MODE ("" == copy)
	xdpAttached bool   // any XDP program on the data interface
	standalone  bool   // that program was attached by us, not by the replicator
	iface       string
}

// readHostState collects everything needed for a convergence decision in ONE
// shell round-trip — this read happens for every role change, so it must not cost
// several sequential exec hops.
func (r *Runner) readHostState() hostState {
	out, _ := sh(`IFACE=$(ip -4 route show default | awk '{print $5}' | head -1)
echo "iface=$IFACE"
echo "active=$(systemctl is-active replicator 2>/dev/null)"
echo "mode=$(grep -o 'REPLICATOR_MODE=[a-z]*' /etc/default/replicator 2>/dev/null | cut -d= -f2)"
echo "fwd=$(grep -o 'REPLICATOR_FWD_MODE=[a-z]*' /etc/default/replicator 2>/dev/null | cut -d= -f2)"
echo "xdp=$(ip -d link show "$IFACE" 2>/dev/null | grep -c 'prog/xdp')"
echo "standalone=$([ -f ` + standaloneMarker + ` ] && echo 1 || echo 0)"`)

	st := hostState{}
	for _, line := range strings.Split(out, "\n") {
		k, v, ok := strings.Cut(strings.TrimSpace(line), "=")
		if !ok {
			continue
		}
		switch k {
		case "iface":
			st.iface = v
		case "active":
			st.svcActive = v == "active"
		case "mode":
			st.mode = v
		case "fwd":
			st.fwd = v
		case "xdp":
			st.xdpAttached = v != "0" && v != ""
		case "standalone":
			st.standalone = v == "1"
		}
	}
	return st
}

// controlPort is the replicator's UDP control protocol port (Replicator::CONTROL_PORT).
const controlPort = 12345

// waitReplicator polls until replicator.service reaches the wanted state, up to
// timeout. When waiting for active it then waits for the control port to be bound,
// which is the point the replicator can serve traffic (it binds the control socket
// last, ~90ms after exec). The bound check reads /proc via `ss` and costs ~2ms, so
// a poll that arrives early is cheap; probing with replicator_ctl instead would
// pay that tool's 5s receive timeout on every miss.
func (r *Runner) waitReplicator(wantActive bool, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		out, _ := sh(`systemctl is-active replicator 2>/dev/null`)
		if (strings.TrimSpace(out) == "active") == wantActive {
			if !wantActive {
				return nil
			}
			for time.Now().Before(deadline) {
				bound, _ := sh(fmt.Sprintf(
					`ss -lunH 'sport = :%d' 2>/dev/null | grep -c . || true`, controlPort))
				if strings.TrimSpace(bound) != "0" && strings.TrimSpace(bound) != "" {
					return nil
				}
				time.Sleep(25 * time.Millisecond)
			}
			return fmt.Errorf("replicator started but control port %d not bound within %s", controlPort, timeout)
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("replicator did not reach active=%v within %s", wantActive, timeout)
		}
		time.Sleep(50 * time.Millisecond)
	}
}

// EnsureHostState converges the host to p.Profile, returning a short description
// of what changed ("no-op ..." when already correct).
func (r *Runner) EnsureHostState(p proto.HostStateParams) (string, error) {
	if p.Profile == "" || p.Profile == proto.HostIdle {
		return "no-op (idle)", nil
	}
	st := r.readHostState()
	if st.iface == "" {
		return "", fmt.Errorf("could not determine data interface")
	}
	switch p.Profile {
	case proto.HostClient:
		return r.ensureClient(st, p.NeedXdpStamp)
	case proto.HostEchoUcast:
		return r.ensureReplicator(st, "ucast", "")
	case proto.HostMcastReplicator:
		return r.ensureReplicator(st, "mcast", p.FwdMode)
	case proto.HostMcastEndpoint:
		return r.ensureEndpoint(st)
	default:
		return "", fmt.Errorf("unknown host profile %q", p.Profile)
	}
}

// ensureClient: replicator stopped, plus the standalone XDP program attached only
// when the measurement needs XDP ingress stamping.
//
// Order matters. The replicator detaches its XDP program when it exits, so the
// standalone attach must come AFTER the stop or it is torn down with the service.
// The standalone program is what keeps xdp mode (--xdp-rx) working: with no program
// attached rtt's RX stamp slot stays zero, every sample fails the 0 < rtt < 100ms
// sanity filter, and the run reports "Lost: 0" with no percentiles at all - which
// unmarshals to a silent and impossible p50=0. The kernel variation stamps via
// SO_TIMESTAMPING and needs no program, so it skips the attach and clears any
// program left behind by an earlier xdp run.
func (r *Runner) ensureClient(st hostState, needXdpStamp bool) (string, error) {
	var did []string

	if st.svcActive {
		if _, err := sh(`sudo systemctl stop replicator`); err != nil {
			return "", fmt.Errorf("stop replicator: %w", err)
		}
		if err := r.waitReplicator(false, 15*time.Second); err != nil {
			return "", err
		}
		did = append(did, "stopped replicator")
	}

	if !needXdpStamp {
		// No ingress stamping required. Clear a standalone program if one is
		// present so a stale attach from an earlier xdp run cannot interfere.
		if st.standalone {
			_, _ = sh(fmt.Sprintf(`sudo bash -c 'ip link set dev %s xdp off 2>/dev/null || true; rm -f %s'`,
				st.iface, standaloneMarker))
			did = append(did, "detached standalone xdp (not stamping)")
		}
		if len(did) == 0 {
			return "no-op (already client)", nil
		}
		return strings.Join(did, " + "), nil
	}

	// Attach only when there is no standalone program already. If the replicator
	// just stopped it took its program with it, so a prior xdpAttached reading is
	// stale — the st.svcActive term forces a re-attach in that case.
	if st.svcActive || !st.xdpAttached || !st.standalone {
		// ONE sudo: each costs ~125ms from this service context.
		out, err := sh(fmt.Sprintf(
			`sudo bash -c 'ip link set dev %s xdp off 2>/dev/null || true
ip link set dev %s xdp obj %s/xdp/ucast.o sec xdp 2>&1 && touch %s'`,
			st.iface, st.iface, r.binDir, standaloneMarker))
		if err != nil {
			return "", fmt.Errorf("attach standalone xdp: %w (%s)", err, strings.TrimSpace(out))
		}
		did = append(did, "attached standalone xdp")
	}

	if len(did) == 0 {
		return "no-op (already client)", nil
	}
	return strings.Join(did, " + "), nil
}

// ensureReplicator: replicator running in `mode` with fan-out `fwd`. Config edits
// and the restart collapse into ONE transition, so changing mode AND fwd together
// costs a single restart rather than two.
func (r *Runner) ensureReplicator(st hostState, mode, fwd string) (string, error) {
	wantFwd := fwd
	if wantFwd == "copy" {
		wantFwd = "" // copy is represented by the variable being absent
	}
	needCfg := st.mode != mode || st.fwd != wantFwd
	// A standalone program must go before the replicator starts, or it competes
	// with the program the replicator attaches itself.
	needDetach := st.standalone

	if !needCfg && !needDetach && st.svcActive {
		return "no-op (already " + mode + "/" + orCopy(wantFwd) + ")", nil
	}

	var did []string
	if needDetach {
		t := time.Now()
		_, _ = sh(fmt.Sprintf(`sudo bash -c 'ip link set dev %s xdp off 2>/dev/null || true; rm -f %s'`,
			st.iface, standaloneMarker))
		did = append(did, fmt.Sprintf("detached standalone xdp(%dms)", time.Since(t).Milliseconds()))
	}
	if needCfg {
		t := time.Now()
		// Compute the new file in Go and write it with ONE sudo. The previous form
		// used four (two sed, two tee); each sudo from a systemd service context
		// costs ~125ms, so the write measured 430-590ms instead of ~125ms.
		keep := []string{}
		if b, err := os.ReadFile(replicatorEnvFile); err == nil {
			for _, ln := range strings.Split(string(b), "\n") {
				s := strings.TrimSpace(ln)
				if s == "" || strings.HasPrefix(s, "REPLICATOR_MODE=") || strings.HasPrefix(s, "REPLICATOR_FWD_MODE=") {
					continue
				}
				keep = append(keep, ln)
			}
		}
		keep = append(keep, "REPLICATOR_MODE="+mode)
		if wantFwd != "" {
			keep = append(keep, "REPLICATOR_FWD_MODE="+wantFwd)
		}
		// Quoted heredoc delimiter: the content is written verbatim, no expansion.
		script := fmt.Sprintf("sudo tee %s >/dev/null <<'AFXDP_EOF'\n%s\nAFXDP_EOF", replicatorEnvFile,
			strings.Join(keep, "\n"))
		if _, err := sh(script); err != nil {
			return "", fmt.Errorf("write replicator config: %w", err)
		}
		did = append(did, fmt.Sprintf("mode=%s fwd=%s cfg(%dms)", mode, orCopy(wantFwd), time.Since(t).Milliseconds()))
	}

	action := "restart"
	if !st.svcActive && !needCfg && !needDetach {
		action = "start"
	}
	tSvc := time.Now()
	if _, err := sh(`sudo systemctl ` + action + ` replicator`); err != nil {
		return "", fmt.Errorf("%s replicator: %w", action, err)
	}
	msSvc := time.Since(tSvc).Milliseconds()
	tReady := time.Now()
	if err := r.waitReplicator(true, 30*time.Second); err != nil {
		return "", err
	}
	did = append(did, fmt.Sprintf("%sed replicator(svc %dms + ready %dms)", action, msSvc, time.Since(tReady).Milliseconds()))
	return strings.Join(did, " + "), nil
}

// ensureEndpoint: mcast source/destination — replicator stopped and the queue
// free, since mcast_send / mcast_receive bind their own AF_XDP sockets and load
// their own program.
func (r *Runner) ensureEndpoint(st hostState) (string, error) {
	var did []string
	if st.svcActive {
		if _, err := sh(`sudo systemctl stop replicator`); err != nil {
			return "", fmt.Errorf("stop replicator: %w", err)
		}
		if err := r.waitReplicator(false, 15*time.Second); err != nil {
			return "", err
		}
		did = append(did, "stopped replicator")
	}
	if st.xdpAttached || st.standalone {
		_, _ = sh(fmt.Sprintf(`sudo bash -c 'ip link set dev %s xdp off 2>/dev/null || true
ip link set dev %s xdpgeneric off 2>/dev/null || true
rm -f %s'`, st.iface, st.iface, standaloneMarker))
		did = append(did, "detached xdp")
	}
	if len(did) == 0 {
		return "no-op (already endpoint)", nil
	}
	return strings.Join(did, " + "), nil
}

func orCopy(s string) string {
	if s == "" {
		return "copy"
	}
	return s
}
