package proto

// ─────────────────────────────────────────────────────────────────────────────
// Node identity / topology — self-reported by the agent from IMDS at register.
// This is exactly the metadata the placement analysis needs (AZ / PG / type),
// captured authoritatively instead of assembled by hand.
// ─────────────────────────────────────────────────────────────────────────────

type NodeInfo struct {
	InstanceID     string `json:"instance_id"`
	PrivateIP      string `json:"private_ip"`
	PublicIP       string `json:"public_ip,omitempty"`
	AZ             string `json:"az"`
	Region         string `json:"region"`
	InstanceType   string `json:"instance_type"`
	PlacementGroup string `json:"placement_group,omitempty"` // "" = no PG
	// Strategy of PlacementGroup: cluster|spread|partition. IMDS exposes only the
	// group name, so this comes from ec2:DescribePlacementGroups.
	PlacementGroupStrategy string `json:"placement_group_strategy,omitempty"`
	VpcID          string `json:"vpc_id,omitempty"`
	SubnetID       string `json:"subnet_id,omitempty"`
	Role           string `json:"role,omitempty"`            // source|replicator|destination
	Stack          string `json:"stack,omitempty"`           // CFN stack name, if tagged
	Hostname       string `json:"hostname,omitempty"`
	// EC2 tenancy: shared (default multi-tenant host), instance (Dedicated
	// Instance), host (Dedicated Host). Self-reported via AGENT_TENANCY (the
	// fleet stack knows the value it requested at launch; IMDS has no native
	// tenancy field to read it back from).
	Tenancy string `json:"tenancy,omitempty"`
	// Hardware specs (populated from EC2 DescribeInstanceTypes at startup).
	VCPUs    int     `json:"vcpus,omitempty"`
	MemGB    float64 `json:"mem_gb,omitempty"`
	BwGbps   float64 `json:"bw_gbps,omitempty"`
	PpsMpps  float64 `json:"pps_mpps,omitempty"`
	ENIs     int     `json:"enis,omitempty"`
	NitroGen string  `json:"nitro_gen,omitempty"`
	Metal    bool    `json:"metal,omitempty"`
}

// Registration is published on every (re)connect. Idempotent: the backend keys
// the registry by InstanceID and upserts.
type Registration struct {
	Node         NodeInfo `json:"node"`
	AgentVersion string   `json:"agent_version"`
	IsolCPUs     string   `json:"isolcpus,omitempty"` // /proc/cmdline isolcpus= value
	StartedUnix  int64    `json:"started_unix"`
}

// Heartbeat is periodic liveness + lightweight state.
type Heartbeat struct {
	InstanceID     string  `json:"instance_id"`
	Unix           int64   `json:"unix"`
	State          string  `json:"state"`                    // idle|running|error
	ReplicatorMode string  `json:"replicator_mode,omitempty"` // ucast|mcast|echo
	ReplicatorSvc  string  `json:"replicator_svc,omitempty"`  // active|inactive
	ClockOffsetUs  float64 `json:"clock_offset_us"`
	CurrentCmdID   string  `json:"current_cmd_id,omitempty"`
}

// ─────────────────────────────────────────────────────────────────────────────
// Commands (backend -> agent) and Results (agent -> backend).
// ─────────────────────────────────────────────────────────────────────────────

type CmdType string

const (
	CmdRunRTT       CmdType = "run_rtt"        // run rtt to a peer, return metrics
	CmdMcastReceive CmdType = "mcast_receive"  // start mcast_receive (foreground), return when done
	CmdMcastRxReady CmdType = "mcast_rx_ready" // is a local mcast_receive attached and listening?
	CmdMcastSend    CmdType = "mcast_send"     // run mcast_send burst
	CmdSetFwdMode   CmdType = "set_fwd_mode"   // set REPLICATOR_FWD_MODE + restart
	CmdSetMode      CmdType = "set_mode"       // set REPLICATOR_MODE (+ fwd) + restart
	CmdReplicatorSvc CmdType = "replicator_svc" // stop|start|restart replicator.service
	CmdJoinGroup    CmdType = "join_group"     // replicator_ctl mcast <group>
	CmdPurgeDests   CmdType = "purge_dests"    // remove stale ucast destinations from the local replicator
	CmdEnsureHost   CmdType = "ensure_host"    // idempotently converge local host state to a measurement profile
	CmdCleanup      CmdType = "cleanup"        // free AF_XDP queue (kill + detach XDP)
	CmdClockSync    CmdType = "clock_sync"     // chronyc makestep + report offset
	CmdStartStream  CmdType = "start_stream"   // continuous rtt --stream to a peer
	CmdStopStream   CmdType = "stop_stream"    // stop the stream
	CmdReregister   CmdType = "reregister"     // re-send Registration (backend restart recovery)
	CmdPing         CmdType = "ping"           // liveness/echo
)

// Command is addressed to an agent (via SubjectCmdAgent/Role/All). The agent
// replies on SubjectResult(InstanceID) with a CommandResult carrying CmdID.
type Command struct {
	ID        CmdType      `json:"-"`      // ignored on wire; kept for readability
	CmdID     string       `json:"cmd_id"` // unique per dispatch (result correlation + idempotency)
	Type      CmdType      `json:"type"`
	RTT       *RTTParams   `json:"rtt,omitempty"`
	Mcast     *McastParams `json:"mcast,omitempty"`
	FwdMode   string       `json:"fwd_mode,omitempty"`   // copy|inplace|bpf_tx (set_fwd_mode / set_mode)
	Mode      string       `json:"mode,omitempty"`       // ucast|mcast|echo (set_mode)
	SvcAction string       `json:"svc_action,omitempty"` // stop|start|restart (replicator_svc)
	Group     string       `json:"group,omitempty"`      // for join_group
	Host      *HostStateParams `json:"host,omitempty"`   // for ensure_host
}

// HostProfile names the desired local host state for a measurement role. Applying
// a profile is IDEMPOTENT: the agent reads current state and performs only the
// transitions that actually differ, so re-applying a profile a node is already in
// costs one cheap read and no service restart.
type HostProfile string

const (
	// HostClient — this node runs `rtt` (the measurer). The replicator is STOPPED
	// so no AF_XDP zero-copy socket owns an RX queue, and the XDP program is
	// attached STANDALONE so --xdp-rx/--xdp-tx still work in xdp mode.
	//
	// Why: a ZC socket bound to the RX queue the returning echoes land on starves
	// the client's ingress. Echoes don't match the replicator's config_map, so
	// XDP_PASS makes the driver copy them out of the UMEM and recycle the frame
	// via the fill queue the replicator's poll loop owns; when that loop isn't
	// draining promptly the NIC drops arriving echoes. Measured: 0-95% loss with
	// the replicator running vs 0% with it stopped — which silently biased every
	// percentile.
	HostClient HostProfile = "client"

	// HostEchoUcast — unicast echo target. Replicator RUNNING in ucast mode; it
	// attaches its own XDP program and binds its own AF_XDP sockets.
	HostEchoUcast HostProfile = "echo-ucast"

	// HostMcastReplicator — replicator RUNNING in mcast mode with FwdMode fan-out.
	HostMcastReplicator HostProfile = "mcast-replicator"

	// HostMcastEndpoint — runs mcast_send / mcast_receive, which bind their own
	// AF_XDP sockets and load their own program. Replicator STOPPED, XDP detached.
	HostMcastEndpoint HostProfile = "mcast-endpoint"

	// HostIdle — no constraint; the agent leaves the host untouched.
	HostIdle HostProfile = "idle"
)

// HostStateParams asks the agent to converge local state to Profile.
type HostStateParams struct {
	Profile HostProfile `json:"profile"`
	FwdMode string      `json:"fwd_mode"` // mcast-replicator only: copy|inplace|bpf_tx

	// NeedXdpStamp applies to HostClient: attach the standalone XDP program so
	// rtt --xdp-rx can stamp at ingress. The kernel variation stamps via
	// SO_TIMESTAMPING instead and needs no program, so leaving this false skips
	// an attach on entry and a detach on restore.
	NeedXdpStamp bool `json:"need_xdp_stamp,omitempty"`
}

// RTTParams mirrors the rtt CLI. XDP flags are the client TX/RX variations.
type RTTParams struct {
	TargetIP   string `json:"target_ip"`
	DataPort   int    `json:"data_port"`
	ListenIP   string `json:"listen_ip"`
	ListenPort int    `json:"listen_port"`
	Count      int    `json:"count"`
	Rate       int    `json:"rate"`
	Warmup     int    `json:"warmup"`
	SendCPU    int    `json:"send_cpu"`  // -1 => derive from isolated set
	RecvCPU    int    `json:"recv_cpu"`  // -1 => derive
	XdpTx      bool   `json:"xdp_tx"`
	XdpTxQueue int    `json:"xdp_tx_queue"`
	XdpRx      bool   `json:"xdp_rx"`

	// MaxLossPct rejects the measurement outright when the observed loss exceeds
	// this percentage. Percentiles are computed ONLY over datagrams that came
	// back, so a lossy run reports the latency of its surviving subset — a
	// survivorship-biased number that is not comparable to a clean run and must
	// never be published as a result. 0 disables the gate (not recommended).
	MaxLossPct float64 `json:"max_loss_pct"`
}

// McastParams mirrors mcast_send / mcast_receive.
type McastParams struct {
	Group        string `json:"group"`
	DataPort     int    `json:"data_port"`
	ReplicatorIP string `json:"replicator_ip,omitempty"` // send target (m2u)
	SourceIP     string `json:"source_ip,omitempty"`     // true source (for dest telemetry edge)
	Count        int    `json:"count"`
	IntervalUs   int    `json:"interval_us"`
	TimeoutSec   int    `json:"timeout_sec"`
	Variation    string `json:"variation,omitempty"` // fwd mode (copy|inplace|bpf_tx|kernel) — tags telemetry

	// Size is the mcast_send payload size in bytes (mirrors `-s`; tool minimum
	// is 32 = WIRE_APP_HDR_LEN). 0 => runner/tool default (64B). mcast_receive
	// has no payload-size flag — it reads whatever size mcast_send sent.
	Size int `json:"size,omitempty"`
	// TxQueue is mcast_send's AF_XDP TX queue (mirrors `-q`). 0 => runner/tool
	// default (queue 1). Queue 0 is RSS-pinned; a ZC TX bind there can wedge
	// the host NIC (see tools/README.md), so 0 is treated as "unset", not
	// "explicitly queue 0".
	TxQueue int `json:"tx_queue,omitempty"`
	// RxQueue is mcast_receive's AF_XDP/XDP queue index (mirrors `-q`). 0 is
	// both "unset" and the tool's own default, so it is always safe to pass.
	RxQueue int `json:"rx_queue,omitempty"`
}

// CommandResult is the ack for a Command. Data carries command-specific output
// (e.g. an rtt/mcast Metrics blob for measurement commands).
type CommandResult struct {
	CmdID      string   `json:"cmd_id"`
	InstanceID string   `json:"instance_id"`
	OK         bool     `json:"ok"`
	Err        string   `json:"err,omitempty"`
	Metrics    *Metrics `json:"metrics,omitempty"`
	Text       string   `json:"text,omitempty"`
}

// SetErr marks the result failed if err is non-nil.
func (r *CommandResult) SetErr(err error) {
	if err != nil {
		r.OK = false
		r.Err = err.Error()
	}
}

// Fail marks the result failed with a message.
func (r *CommandResult) Fail(msg string) {
	r.OK = false
	r.Err = msg
}

// ─────────────────────────────────────────────────────────────────────────────
// Telemetry — one measurement sample/window (matches the service_rtt_us schema
// the C++ tools already emit). Streamed for live monitoring and stored for reports.
// ─────────────────────────────────────────────────────────────────────────────

type Pct struct {
	Min  int64 `json:"min"`
	Mean int64 `json:"mean"`
	P50  int64 `json:"p50"`
	P90  int64 `json:"p90"`
	P95  int64 `json:"p95"`
	P99  int64 `json:"p99"`
	P999 int64 `json:"p999"`
	Max  int64 `json:"max"`
}

// HopPct is a small percentile set (p50/p99/p999), matching what
// mcast_receive.cpp actually computes for a hop breakdown - a narrower set
// than Pct's, which is for the full service_rtt_us statistic.
type HopPct struct {
	P50  int64 `json:"p50"`
	P99  int64 `json:"p99"`
	P999 int64 `json:"p999"`
}

type Metrics struct {
	ServiceRTT       Pct     `json:"service_rtt_us"`
	Messages         int64   `json:"messages"`
	Lost             int64   `json:"lost"`
	LossPct          float64 `json:"loss_pct"`
	ClockSkewSamples int64   `json:"clock_skew_samples,omitempty"`
	// Hop1/Hop2 split the mcast one-way path into source->replicator and
	// replicator->destination legs. mcast-only: nil for every ucast/rtt
	// result, and nil for mcast too unless the wire header carried a
	// replicator timestamp (mcast_receive.cpp's has_replicator_ts).
	Hop1 *HopPct `json:"hop1_us,omitempty"`
	Hop2 *HopPct `json:"hop2_us,omitempty"`
}

// ErrorEvent is published proactively by an agent when a command fails or an
// internal error is detected (XDP detach, clock drift, service crash). The
// backend stores a bounded per-node error ring and broadcasts to the web UI.
type ErrorEvent struct {
	InstanceID string `json:"instance_id"`
	Unix       int64  `json:"unix"`
	CmdID      string `json:"cmd_id,omitempty"` // correlates with the failed command, if any
	CmdType    string `json:"cmd_type,omitempty"`
	Error      string `json:"error"`
	Context    string `json:"context,omitempty"` // extra detail (stderr snippet, etc.)
}

// Telemetry is a directed edge measurement (src -> dst), tagged with the
// producing agent so the backend can place it in the NxN matrix live.
type Telemetry struct {
	InstanceID string  `json:"instance_id"` // producer (sender)
	Unix       int64   `json:"unix"`
	Kind       string  `json:"kind"`   // ucast|mcast
	SrcIP      string  `json:"src_ip"`
	DstIP      string  `json:"dst_ip"`
	Variation  string  `json:"variation,omitempty"` // kernel|xdp|copy|inplace
	TxMode     string  `json:"tx_mode,omitempty"`   // AF_XDP TX bind: zero-copy|copy ("" = kernel TX)
	CmdID      string  `json:"cmd_id,omitempty"`
	Metrics    Metrics `json:"metrics"`
}
