package ingest

import (
	"encoding/json"
	"log"

	"afxdp-cp/backend/collector"
	"afxdp-cp/backend/hub"
	"afxdp-cp/backend/registry"
	"afxdp-cp/backend/store"
	"afxdp-cp/proto"

	"github.com/nats-io/nats.go"
)

// reregisterCmd is a pre-marshaled nudge sent to a node the backend doesn't
// know (e.g. after a restart) so it re-sends its Registration immediately.
var reregisterCmd, _ = json.Marshal(proto.Command{Type: proto.CmdReregister})

// StartIngest wires the agent-outbound streams into the registry, collector,
// and SSE hub. This is the read side; the orchestrator is the write side.
func StartIngest(nc *nats.Conn, reg *registry.Registry, coll *collector.Collector, hub *hub.Hub, st *store.Store) error {
	if _, err := nc.Subscribe(proto.SubjectRegister, func(m *nats.Msg) {
		var r proto.Registration
		if json.Unmarshal(m.Data, &r) == nil {
			n := reg.Upsert(r)
			log.Printf("register: %s (%s, %s, pg=%q)", n.InstanceID, n.PrivateIP, n.AZ, n.PlacementGroup)
			hub.Emit("node", n)
		}
	}); err != nil {
		return err
	}
	if _, err := nc.Subscribe(proto.SubjectHeartbeat, func(m *nats.Msg) {
		var hb proto.Heartbeat
		if json.Unmarshal(m.Data, &hb) != nil {
			return
		}
		n, changed := reg.Heartbeat(hb)
		if n == nil {
			// Unknown node (e.g. after a backend restart) — nudge a re-register
			// so it repopulates immediately instead of waiting for its periodic one.
			nc.Publish(proto.SubjectCmdAgent(hb.InstanceID), reregisterCmd)
			return
		}
		if changed { // suppress the every-tick broadcast; only emit on real change
			hub.Emit("node", n)
		}
	}); err != nil {
		return err
	}
	if _, err := nc.Subscribe(proto.SubjectTelemetry, func(m *nats.Msg) {
		var t proto.Telemetry
		if json.Unmarshal(m.Data, &t) == nil {
			e := coll.Apply(t)
			hub.Emit("edge", e)
			store.RecordMeasurement(st, t, st.CurrentRun())
		}
	}); err != nil {
		return err
	}
	return nil
}
