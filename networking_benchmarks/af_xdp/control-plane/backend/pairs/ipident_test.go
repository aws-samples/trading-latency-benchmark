package pairs

import (
	"strings"
	"testing"

	"afxdp-cp/backend/registry"
	"afxdp-cp/proto"
)

// The web identifies nodes by private IP: the matrix, the edges and the pinned
// tables are all keyed that way, and the live view model does not carry the
// instance id at all. Matching the target set on instance id alone therefore
// rejected every selection the UI could make, with the misleading message
// "none of the N requested node(s) are online" listing addresses that were
// plainly online.
func TestResolvePairsAcceptsPrivateIPs(t *testing.T) {
	nodes := []registry.Node{
		{NodeInfo: proto.NodeInfo{InstanceID: "i-aaa", PrivateIP: "10.61.0.217"}, Online: true},
		{NodeInfo: proto.NodeInfo{InstanceID: "i-bbb", PrivateIP: "10.61.0.149"}, Online: true},
		{NodeInfo: proto.NodeInfo{InstanceID: "i-ccc", PrivateIP: "10.61.0.43"}, Online: true},
	}

	t.Run("private IPs resolve", func(t *testing.T) {
		// Exactly the payload the web UI sends today.
		sources, destsFor, skipped, err := ResolvePairs(nodes, []string{"10.61.0.217", "10.61.0.149"}, ScopeAmong)
		if err != nil {
			t.Fatalf("private IPs must resolve, got: %v", err)
		}
		if len(skipped) != 0 {
			t.Fatalf("nothing should be skipped, got %v", skipped)
		}
		got := pairsOf(sources, destsFor)
		want := []string{"i-aaa>i-bbb", "i-bbb>i-aaa"}
		if strings.Join(got, ",") != strings.Join(want, ",") {
			t.Fatalf("pairs = %v, want %v", got, want)
		}
	})

	t.Run("instance ids still resolve", func(t *testing.T) {
		sources, destsFor, _, err := ResolvePairs(nodes, []string{"i-aaa", "i-bbb"}, ScopeAmong)
		if err != nil {
			t.Fatalf("instance ids must keep working: %v", err)
		}
		want := []string{"i-aaa>i-bbb", "i-bbb>i-aaa"}
		if got := pairsOf(sources, destsFor); strings.Join(got, ",") != strings.Join(want, ",") {
			t.Fatalf("pairs = %v, want %v", got, want)
		}
	})

	t.Run("mixed ids and IPs resolve", func(t *testing.T) {
		sources, destsFor, _, err := ResolvePairs(nodes, []string{"i-aaa", "10.61.0.149"}, ScopeAmong)
		if err != nil {
			t.Fatalf("a mixed target set must resolve: %v", err)
		}
		if n := len(pairsOf(sources, destsFor)); n != 2 {
			t.Fatalf("pairs = %d, want 2", n)
		}
	})

	t.Run("the same node named twice is de-duplicated", func(t *testing.T) {
		// Its id AND its IP: still one node, so among yields 0 pairs and errors
		// rather than inventing a self-pair.
		_, _, _, err := ResolvePairs(nodes, []string{"i-aaa", "10.61.0.217"}, ScopeAmong)
		if err == nil {
			t.Fatal("one node addressed two ways must not resolve to a pair")
		}
	})

	t.Run("fanout by IP", func(t *testing.T) {
		sources, destsFor, _, err := ResolvePairs(nodes, []string{"10.61.0.217"}, ScopeFanout)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		want := []string{"i-aaa>i-bbb", "i-aaa>i-ccc"}
		if got := pairsOf(sources, destsFor); strings.Join(got, ",") != strings.Join(want, ",") {
			t.Fatalf("pairs = %v, want %v", got, want)
		}
	})

	t.Run("an offline node named by IP is still skipped", func(t *testing.T) {
		off := make([]registry.Node, len(nodes))
		copy(off, nodes)
		off[1].Online = false
		_, _, skipped, err := ResolvePairs(off, []string{"10.61.0.217", "10.61.0.149"}, ScopeAmong)
		if err == nil {
			t.Fatal("one online node under among must error")
		}
		if strings.Join(skipped, ",") != "10.61.0.149" {
			t.Fatalf("skipped = %v, want the offline IP reported back as given", skipped)
		}
	})

	t.Run("an unknown address is still reported", func(t *testing.T) {
		_, _, skipped, err := ResolvePairs(nodes, []string{"10.61.0.217", "10.99.99.99"}, ScopeAmong)
		if err == nil {
			t.Fatal("only one resolvable node under among must error")
		}
		if strings.Join(skipped, ",") != "10.99.99.99" {
			t.Fatalf("skipped = %v, want [10.99.99.99]", skipped)
		}
	})
}
