// Package proto is the shared wire contract between the agent (on each fleet
// node) and the central backend. It is the single source of truth for NATS
// subjects and message schemas, so agent and backend cannot drift.
//
// Communication is agent-outbound only: every node holds one persistent NATS
// connection to the backend's NATS endpoint. No inbound ports are opened on
// measurement nodes (this is what removes the SSH/ICMP SG friction and the
// AF_XDP-on-the-SSH-NIC disruption from the control path).
package proto

// Subjects. Agents PUBLISH register/heartbeat/telemetry/result; agents
// SUBSCRIBE to the command subjects addressed to them (all / role / id).
// The backend does the mirror.
const (
	SubjectRegister  = "fleet.register"  // agent -> backend, once per (re)connect
	SubjectHeartbeat = "fleet.heartbeat" // agent -> backend, periodic liveness
	SubjectTelemetry = "fleet.telemetry" // agent -> backend, measurement stream
	SubjectError     = "fleet.error"     // agent -> backend, error event (proactive)
	SubjectCmdAll    = "fleet.cmd.all"   // backend -> every agent (broadcast)

	// Wildcards the backend subscribes to.
	SubjectResultWildcard    = "fleet.result.*"
	SubjectTelemetryWildcard = "fleet.telemetry" // single subject; agents tag payload with AgentID
)

// SubjectCmdAgent is the per-agent command inbox (backend -> one agent).
func SubjectCmdAgent(agentID string) string { return "fleet.cmd.agent." + agentID }

// SubjectCmdRole targets every agent of a role, e.g. "replicator" (backend -> role).
func SubjectCmdRole(role string) string { return "fleet.cmd.role." + role }

// SubjectResult is where an agent publishes the ack/result for a command.
func SubjectResult(agentID string) string { return "fleet.result." + agentID }
