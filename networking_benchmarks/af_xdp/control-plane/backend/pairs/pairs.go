package pairs

import (
	"fmt"

	"afxdp-cp/backend/registry"
)

// Scope names accepted by ResolvePairs. They expand a target set into ordered
// pairs; an empty target set is a full mesh regardless of scope.
const (
	ScopeAmong  = "among"  // k*(k-1): how the selected nodes see each other
	ScopeFanout = "fanout" // k*(N-1): how the selected nodes reach the fleet
	ScopeFanin  = "fanin"  // (N-1)*k: how the fleet reaches the selected nodes
)

// ResolvePairs turns a target set + scope into the (sources, destsFor) shape the
// source-grouped campaign loop needs.
//
// Offline and unknown ids are dropped and returned in skipped so the caller can
// say so rather than silently measuring less than the user asked for. An empty
// target set means full mesh, which is the pre-existing behaviour.
func ResolvePairs(online []registry.Node, ids []string, scope string) (
	sources []registry.Node, destsFor map[string][]registry.Node, skipped []string, err error) {

	if scope == "" {
		scope = ScopeAmong
	}
	if scope != ScopeAmong && scope != ScopeFanout && scope != ScopeFanin {
		return nil, nil, nil, fmt.Errorf("unknown scope %q (want %s, %s or %s)",
			scope, ScopeAmong, ScopeFanout, ScopeFanin)
	}

	// Only online nodes are ever measurable. A node that dropped out between the
	// UI selecting it and the campaign starting must not be dispatched to.
	up := make([]registry.Node, 0, len(online))
	byID := map[string]registry.Node{}
	for _, n := range online {
		if !n.Online {
			continue
		}
		up = append(up, n)
		byID[n.InstanceID] = n
	}

	// Resolve the target set, preserving the fleet's own ordering so a run is
	// deterministic regardless of the order the user clicked nodes in.
	var targets []registry.Node
	if len(ids) > 0 {
		want := map[string]bool{}
		for _, id := range ids {
			want[id] = true
		}
		for _, n := range up {
			if want[n.InstanceID] {
				targets = append(targets, n)
				delete(want, n.InstanceID) // de-duplicate repeated ids
			}
		}
		for id := range want {
			skipped = append(skipped, id) // offline or not in the fleet at all
		}
	}

	destsFor = map[string][]registry.Node{}
	// exclSelf appends every node in pool except src.
	exclSelf := func(src registry.Node, pool []registry.Node) []registry.Node {
		out := make([]registry.Node, 0, len(pool))
		for _, d := range pool {
			if d.InstanceID != src.InstanceID {
				out = append(out, d)
			}
		}
		return out
	}

	// A caller that asked for specific nodes must never silently widen to the
	// full mesh when none of them resolve: that would run every pair in the
	// fleet instead of the handful requested.
	if len(ids) > 0 && len(targets) == 0 {
		return nil, nil, skipped, fmt.Errorf(
			"none of the %d requested node(s) are online: %v", len(ids), skipped)
	}

	switch {
	case len(ids) == 0:
		// Full mesh. Scope is meaningless here and deliberately ignored.
		if len(up) < 2 {
			return nil, nil, skipped, fmt.Errorf("need >=2 online nodes for a full mesh, have %d", len(up))
		}
		sources = up
		for _, s := range up {
			destsFor[s.InstanceID] = exclSelf(s, up)
		}
	case scope == ScopeAmong:
		sources = targets
		for _, s := range targets {
			destsFor[s.InstanceID] = exclSelf(s, targets)
		}
	case scope == ScopeFanout:
		sources = targets
		for _, s := range targets {
			destsFor[s.InstanceID] = exclSelf(s, up)
		}
	case scope == ScopeFanin:
		// Every online node measures to the targets. A source with no dests left
		// (it is the only target) is omitted so the loop does not pay a profile
		// transition for zero measurements.
		for _, s := range up {
			if d := exclSelf(s, targets); len(d) > 0 {
				sources = append(sources, s)
				destsFor[s.InstanceID] = d
			}
		}
	}

	n := 0
	for _, s := range sources {
		n += len(destsFor[s.InstanceID])
	}
	if n == 0 {
		return nil, nil, skipped, fmt.Errorf(
			"scope %q over %d selected node(s) resolves to 0 pairs", scope, len(targets))
	}
	return sources, destsFor, skipped, nil
}

// PrepareSet is the union of sources and every destination.
//
// The prepare phase converges hosts to the echo profile, and it must cover
// destinations too: a node left in client profile by an earlier run does not
// echo, so measuring to it would fail for a reason unrelated to the network.
func PrepareSet(sources []registry.Node, destsFor map[string][]registry.Node) []registry.Node {
	seen := map[string]bool{}
	var out []registry.Node
	add := func(n registry.Node) {
		if !seen[n.InstanceID] {
			seen[n.InstanceID] = true
			out = append(out, n)
		}
	}
	for _, s := range sources {
		add(s)
		for _, d := range destsFor[s.InstanceID] {
			add(d)
		}
	}
	return out
}

// ScopeDescription renders the resolved scope for the job event, so the UI log
// states what ran instead of only how many pairs it was.
func ScopeDescription(scope string, k int) string {
	if k == 0 {
		return "full mesh"
	}
	nodeWord := "nodes"
	if k == 1 {
		nodeWord = "node"
	}
	switch scope {
	case ScopeFanout:
		return fmt.Sprintf("%d selected %s to all", k, nodeWord)
	case ScopeFanin:
		return fmt.Sprintf("all to %d selected %s", k, nodeWord)
	default:
		return fmt.Sprintf("among %d selected %s", k, nodeWord)
	}
}

// CountPairs predicts the pair count without a fleet snapshot, for the UI button
// label. It mirrors ResolvePairs and returns 0 where that would resolve to
// nothing, so the label never promises a run that will be refused.
//
// fanout and fanin are both k*(N-1): a fanin source that is itself a target
// contributes k-1 dests rather than k, and k*(N-k) + k*(k-1) reduces to k*(N-1).
func CountPairs(n, k int, scope string) int {
	if n < 2 {
		return 0
	}
	if k == 0 {
		return n * (n - 1) // full mesh; scope is ignored
	}
	if k > n {
		k = n
	}
	switch scope {
	case ScopeFanout, ScopeFanin:
		return k * (n - 1)
	default: // among
		return k * (k - 1)
	}
}

// ScopeName is the scope as persisted in the runs table: "full" for a full mesh,
// otherwise the requested scope id. Distinct from ScopeDescription, which is
// prose for the UI log.
func ScopeName(scope string, k int) string {
	if k == 0 {
		return "full"
	}
	if scope == "" {
		return ScopeAmong
	}
	return scope
}
