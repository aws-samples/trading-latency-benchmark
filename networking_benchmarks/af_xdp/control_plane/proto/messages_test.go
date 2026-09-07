package proto

import (
	"encoding/json"
	"errors"
	"testing"
)

func TestCommandRoundTrip(t *testing.T) {
	in := Command{
		CmdID: "abc", Type: CmdRunRTT,
		RTT: &RTTParams{TargetIP: "10.0.0.2", DataPort: 5000, ListenIP: "10.0.0.1",
			ListenPort: 19020, Count: 10000, Rate: 10000, Warmup: 1000, SendCPU: -1, RecvCPU: -1, XdpTx: true, XdpTxQueue: 1},
	}
	b, err := json.Marshal(in)
	if err != nil {
		t.Fatal(err)
	}
	var out Command
	if err := json.Unmarshal(b, &out); err != nil {
		t.Fatal(err)
	}
	if out.Type != CmdRunRTT || out.RTT == nil || out.RTT.TargetIP != "10.0.0.2" || !out.RTT.XdpTx {
		t.Fatalf("round-trip mismatch: %+v", out)
	}
	if out.RTT.SendCPU != -1 {
		t.Fatalf("SendCPU sentinel lost: %d", out.RTT.SendCPU)
	}
}

func TestMcastCommandRoundTrip(t *testing.T) {
	in := Command{CmdID: "m1", Type: CmdMcastReceive, Mcast: &McastParams{
		Group: "224.0.31.50", DataPort: 5000, ReplicatorIP: "10.0.0.5", SourceIP: "10.0.0.1",
		Count: 1000, TimeoutSec: 60, Variation: "inplace"}}
	b, _ := json.Marshal(in)
	var out Command
	if err := json.Unmarshal(b, &out); err != nil {
		t.Fatal(err)
	}
	if out.Mcast == nil || out.Mcast.Variation != "inplace" || out.Mcast.SourceIP != "10.0.0.1" {
		t.Fatalf("mcast round-trip mismatch: %+v", out.Mcast)
	}
}

func TestCommandResultHelpers(t *testing.T) {
	r := CommandResult{OK: true}
	r.SetErr(nil)
	if !r.OK {
		t.Fatal("SetErr(nil) should not fail the result")
	}
	r.SetErr(errors.New("boom"))
	if r.OK || r.Err != "boom" {
		t.Fatalf("SetErr(err) not applied: %+v", r)
	}
	r2 := CommandResult{OK: true}
	r2.Fail("nope")
	if r2.OK || r2.Err != "nope" {
		t.Fatalf("Fail not applied: %+v", r2)
	}
}
