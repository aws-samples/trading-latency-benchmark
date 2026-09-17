// Command afxdp-agent runs on each fleet node. It self-registers via IMDS, holds
// one persistent OUTBOUND NATS connection to the backend, heartbeats, and
// executes measurement commands by driving the C++ tools in /opt/af-xdp,
// streaming results + telemetry back. No inbound ports are opened here.
package main

import (
	"crypto/tls"
	"encoding/json"
	"flag"
	"log"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"syscall"
	"time"

	"afxdp-cp/proto"

	"github.com/nats-io/nats.go"
)

const agentVersion = "0.1.0"

type agent struct {
	nc   *nats.Conn
	run  *Runner
	node proto.NodeInfo

	execMu sync.Mutex // serialize command execution (shared /tmp files + one AF_XDP queue)
	mu     sync.Mutex
	state  string // idle|running|error
	curCmd string
}

func main() {
	natsURL := flag.String("nats", envOr("AGENT_NATS_URL", nats.DefaultURL), "NATS server URL")
	binDir := flag.String("bindir", envOr("AGENT_BIN_DIR", "/opt/af-xdp"), "directory of the C++ tools")
	hb := flag.Int("heartbeat", 5, "heartbeat interval seconds")
	flag.Parse()

	node := gatherNodeInfo()
	enrichFromEC2(&node)
	enrichPlacementGroup(&node)
	a := &agent{run: NewRunner(*binDir), node: node, state: "idle"}
	log.Printf("agent %s: instance=%s ip=%s az=%s type=%s pg=%q role=%q",
		agentVersion, node.InstanceID, node.PrivateIP, node.AZ, node.InstanceType, node.PlacementGroup, node.Role)

	nodeOpts := []nats.Option{
		nats.Name("afxdp-agent/" + node.InstanceID),
		nats.MaxReconnects(-1), // reconnect forever
		nats.ReconnectWait(2 * time.Second),
		nats.ReconnectHandler(func(c *nats.Conn) { log.Printf("reconnected to %s; re-registering", c.ConnectedUrl()); a.register() }),
		nats.DisconnectErrHandler(func(_ *nats.Conn, e error) { log.Printf("disconnected: %v", e) }),
	}
	if tok := os.Getenv("AGENT_NATS_TOKEN"); tok != "" {
		nodeOpts = append(nodeOpts, nats.Token(tok))
	}
	if ca := os.Getenv("AGENT_NATS_CA"); ca != "" {
		nodeOpts = append(nodeOpts, nats.RootCAs(ca)) // validate server cert against a CA
	} else if os.Getenv("AGENT_NATS_INSECURE") != "" {
		nodeOpts = append(nodeOpts, nats.Secure(&tls.Config{InsecureSkipVerify: true})) // self-signed lab cert
	}
	nc, err := nats.Connect(*natsURL, nodeOpts...)
	if err != nil {
		log.Fatalf("nats connect %s: %v", *natsURL, err)
	}
	a.nc = nc
	defer nc.Drain()

	// Subscribe to command subjects addressed to this agent: all / role / id.
	mustSub(nc, proto.SubjectCmdAll, a.onCommand)
	if node.Role != "" {
		mustSub(nc, proto.SubjectCmdRole(node.Role), a.onCommand)
	}
	mustSub(nc, proto.SubjectCmdAgent(node.InstanceID), a.onCommand)

	a.register()
	go a.heartbeatLoop(time.Duration(*hb) * time.Second)

	log.Printf("agent ready; listening for commands on %s / %s / %s",
		proto.SubjectCmdAll, proto.SubjectCmdRole(node.Role), proto.SubjectCmdAgent(node.InstanceID))

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Printf("shutting down")
}

func (a *agent) register() {
	a.publish(proto.SubjectRegister, proto.Registration{
		Node: a.node, AgentVersion: agentVersion, IsolCPUs: readIsolcpus(), StartedUnix: nowUnix(),
	})
}

func (a *agent) heartbeatLoop(d time.Duration) {
	t := time.NewTicker(d)
	defer t.Stop()
	beat := 0
	for range t.C {
		a.mu.Lock()
		st, cur := a.state, a.curCmd
		a.mu.Unlock()
		a.publish(proto.SubjectHeartbeat, proto.Heartbeat{
			InstanceID: a.node.InstanceID, Unix: nowUnix(), State: st,
			ReplicatorMode: a.run.ReplicatorMode(), ReplicatorSvc: a.run.ReplicatorActive(),
			ClockOffsetUs: clockOffsetUs(), CurrentCmdID: cur,
		})
		// Periodically re-register so a restarted backend (with NATS still up,
		// so no client reconnect fires) repopulates its registry within ~15s.
		if beat++; beat%5 == 0 {
			a.register()
		}
	}
}

func (a *agent) setState(st, cmdID string) {
	a.mu.Lock()
	a.state, a.curCmd = st, cmdID
	a.mu.Unlock()
}

// onCommand handles a Command. The NATS client dispatches a subscription's
// callbacks SEQUENTIALLY, so this returns immediately and does the work in a
// goroutine; otherwise a long blocking command (CmdMcastReceive runs for the
// whole receive) would stall every later command on the same subscription,
// including read-only status probes. execMu below still serialises real work.
func (a *agent) onCommand(m *nats.Msg) {
	var c proto.Command
	if err := json.Unmarshal(m.Data, &c); err != nil {
		log.Printf("bad command: %v", err)
		return
	}
	go a.handleCommand(c)
}

func (a *agent) handleCommand(c proto.Command) {
	// Read-only status queries answer WITHOUT taking execMu. They touch no shared
	// state and need no AF_XDP queue, and the command they report on is itself
	// holding the lock: CmdMcastReceive blocks for the whole receive, so a probe
	// that waited for the lock could never be answered while it mattered.
	if c.Type == proto.CmdMcastRxReady {
		res := proto.CommandResult{CmdID: c.CmdID, InstanceID: a.node.InstanceID, OK: true}
		if !a.run.McastRxReady() {
			res.Fail("mcast_receive not listening yet")
		}
		a.publish(proto.SubjectResult(a.node.InstanceID), res)
		return
	}

	// Serialize: the node has ONE AF_XDP queue and fixed /tmp result files, so
	// two commands must never execute concurrently (NATS delivers cmd.all /
	// cmd.role / cmd.agent on separate goroutines).
	a.execMu.Lock()
	defer a.execMu.Unlock()
	a.setState("running", c.CmdID)
	defer a.setState("idle", "")

	res := proto.CommandResult{CmdID: c.CmdID, InstanceID: a.node.InstanceID, OK: true}
	switch c.Type {
	case proto.CmdPing:
		res.Text = "pong " + agentVersion
	case proto.CmdReregister:
		a.register()
		res.Text = "re-registered"
	case proto.CmdCleanup:
		res.SetErr(a.run.FreeQueue())
	case proto.CmdClockSync:
		off, err := a.run.ClockSync()
		res.SetErr(err)
		res.Text = formatFloat(off)
	case proto.CmdSetFwdMode:
		res.SetErr(a.run.SetFwdMode(c.FwdMode))
	case proto.CmdSetMode:
		res.SetErr(a.run.SetMode(c.Mode, c.FwdMode))
	case proto.CmdReplicatorSvc:
		res.SetErr(a.run.ReplicatorSvc(c.SvcAction))
	case proto.CmdJoinGroup:
		if c.Mcast == nil {
			res.Fail("join_group requires mcast params")
		} else {
			res.SetErr(a.run.JoinGroup(c.Mcast.ReplicatorIP, c.Mcast.Group))
		}
	case proto.CmdPurgeDests:
		res.SetErr(a.run.PurgeDests())
	case proto.CmdEnsureHost:
		if c.Host == nil {
			res.Fail("ensure_host requires host params")
			break
		}
		if what, err := a.run.EnsureHostState(*c.Host); err != nil {
			res.SetErr(err)
		} else {
			res.Text = what
		}
	case proto.CmdRunRTT:
		if c.RTT == nil {
			res.Fail("run_rtt requires rtt params")
			break
		}
		mx, txMode, err := a.run.RunRTT(*c.RTT)
		if err != nil {
			res.SetErr(err)
			break
		}
		res.Metrics = &mx
		res.Text = txMode
		a.publish(proto.SubjectTelemetry, proto.Telemetry{
			InstanceID: a.node.InstanceID, Unix: nowUnix(), Kind: "ucast",
			SrcIP: a.node.PrivateIP, DstIP: c.RTT.TargetIP, Variation: rttVariation(c.RTT),
			CmdID: c.CmdID, Metrics: mx, TxMode: txMode,
		})
	case proto.CmdMcastReceive:
		if c.Mcast == nil {
			res.Fail("mcast_receive requires mcast params")
			break
		}
		mx, err := a.run.RunMcastReceive(*c.Mcast)
		if err != nil {
			res.SetErr(err)
			break
		}
		res.Metrics = &mx
		src := c.Mcast.SourceIP
		if src == "" {
			src = c.Mcast.ReplicatorIP
		}
		a.publish(proto.SubjectTelemetry, proto.Telemetry{
			InstanceID: a.node.InstanceID, Unix: nowUnix(), Kind: "mcast",
			SrcIP: src, DstIP: a.node.PrivateIP, Variation: c.Mcast.Variation, CmdID: c.CmdID, Metrics: mx,
		})
	case proto.CmdMcastSend:
		if c.Mcast == nil {
			res.Fail("mcast_send requires mcast params")
		} else {
			res.SetErr(a.run.RunMcastSend(*c.Mcast))
		}
	default:
		res.Fail("unknown command type: " + string(c.Type))
	}
	a.publish(proto.SubjectResult(a.node.InstanceID), res)
	// Proactively report errors to the backend error registry.
	if !res.OK && res.Err != "" {
		a.publish(proto.SubjectError, proto.ErrorEvent{
			InstanceID: a.node.InstanceID,
			Unix:       time.Now().Unix(),
			CmdID:      c.CmdID,
			CmdType:    string(c.Type),
			Error:      res.Err,
		})
	}
}

func (a *agent) publish(subject string, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		log.Printf("marshal %s: %v", subject, err)
		return
	}
	if err := a.nc.Publish(subject, b); err != nil {
		log.Printf("publish %s: %v", subject, err)
	}
}

// ── small helpers ────────────────────────────────────────────────────────────

func rttVariation(p *proto.RTTParams) string {
	if p.XdpTx || p.XdpRx {
		return "xdp"
	}
	return "kernel"
}

func mustSub(nc *nats.Conn, subj string, cb nats.MsgHandler) {
	if _, err := nc.Subscribe(subj, cb); err != nil {
		log.Fatalf("subscribe %s: %v", subj, err)
	}
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func formatFloat(f float64) string { return strconv.FormatFloat(f, 'f', 3, 64) }
