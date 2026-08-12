package main

import (
	"fmt"
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

// hostState is the observed local configuration.
type hostState struct {
	svcActive   bool   // replicator.service is active
	mode        string // REPLICATOR_MODE (ucast|mcast|kernel)
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

// waitReplicator polls until replicator.service reaches the wanted state, up to
// timeout. Polling beats a fixed `sleep 6`: the common case returns in well under
// a second, while a genuinely slow start still gets its full budget.
func waitReplicator(wantActive bool, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		out, _ := sh(`systemctl is-active replicator 2>/dev/null`)
		if (strings.TrimSpace(out) == "active") == wantActive {
			if wantActive {
				// A freshly started replicator needs a moment to bind its AF_XDP
				// sockets and attach its program before it will echo.
				time.Sleep(1500 * time.Millisecond)
			}
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("replicator did not reach active=%v within %s", wantActive, timeout)
		}
		time.Sleep(250 * time.Millisecond)
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
		return r.ensureClient(st)
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

// ensureClient: replicator stopped + XDP attached standalone.
//
// Order matters. The replicator detaches its XDP program when it exits, so the
// standalone attach must come AFTER the stop or it is torn down with the service.
// The standalone program is what keeps xdp mode (--xdp-rx) working: with no program attached
// rtt's RX stamp slot stays zero, every sample fails the 0 < rtt < 100ms sanity
// filter, and the run reports "Lost: 0" with no percentiles at all — which
// unmarshals to a silent and impossible p50=0.
func (r *Runner) ensureClient(st hostState) (string, error) {
	var did []string

	if st.svcActive {
		if _, err := sh(`sudo systemctl stop replicator`); err != nil {
			return "", fmt.Errorf("stop replicator: %w", err)
		}
		if err := waitReplicator(false, 15*time.Second); err != nil {
			return "", err
		}
		did = append(did, "stopped replicator")
	}

	// Attach only when there is no standalone program already. If the replicator
	// just stopped it took its program with it, so a prior xdpAttached reading is
	// stale — the st.svcActive term forces a re-attach in that case.
	if st.svcActive || !st.xdpAttached || !st.standalone {
		out, err := sh(fmt.Sprintf(
			`sudo ip link set dev %s xdp off 2>/dev/null || true
sudo ip link set dev %s xdp obj %s/xdp/ucast.o sec xdp 2>&1 && sudo touch %s`,
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
		_, _ = sh(fmt.Sprintf(`sudo ip link set dev %s xdp off 2>/dev/null || true; sudo rm -f %s`,
			st.iface, standaloneMarker))
		did = append(did, "detached standalone xdp")
	}
	if needCfg {
		script := fmt.Sprintf(`sudo sed -i '/^REPLICATOR_MODE=/d' /etc/default/replicator
echo REPLICATOR_MODE=%s | sudo tee -a /etc/default/replicator >/dev/null
sudo sed -i '/^REPLICATOR_FWD_MODE=/d' /etc/default/replicator`, mode)
		if wantFwd != "" {
			script += fmt.Sprintf("\necho REPLICATOR_FWD_MODE=%s | sudo tee -a /etc/default/replicator >/dev/null", wantFwd)
		}
		if _, err := sh(script); err != nil {
			return "", fmt.Errorf("write replicator config: %w", err)
		}
		did = append(did, "mode="+mode+" fwd="+orCopy(wantFwd))
	}

	action := "restart"
	if !st.svcActive && !needCfg && !needDetach {
		action = "start"
	}
	if _, err := sh(`sudo systemctl ` + action + ` replicator`); err != nil {
		return "", fmt.Errorf("%s replicator: %w", action, err)
	}
	if err := waitReplicator(true, 30*time.Second); err != nil {
		return "", err
	}
	did = append(did, action+"ed replicator")
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
		if err := waitReplicator(false, 15*time.Second); err != nil {
			return "", err
		}
		did = append(did, "stopped replicator")
	}
	if st.xdpAttached || st.standalone {
		_, _ = sh(fmt.Sprintf(`sudo ip link set dev %s xdp off 2>/dev/null || true
sudo ip link set dev %s xdpgeneric off 2>/dev/null || true
sudo rm -f %s`, st.iface, st.iface, standaloneMarker))
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
