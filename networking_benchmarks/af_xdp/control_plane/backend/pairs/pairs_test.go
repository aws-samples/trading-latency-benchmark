package pairs

import (
	"encoding/json"
	"sort"
	"strings"
	"testing"

	"afxdp-cp/backend/registry"
	"afxdp-cp/proto"
)

// fleet builds n online nodes i-1..i-n with matching 10.0.0.x private IPs.
func fleet(n int) []registry.Node {
	out := make([]registry.Node, 0, n)
	for i := 1; i <= n; i++ {
		out = append(out, registry.Node{
			NodeInfo: proto.NodeInfo{
				InstanceID: "i-" + string(rune('0'+i)),
				PrivateIP:  "10.0.0." + string(rune('0'+i)),
			},
			Online: true,
		})
	}
	return out
}

// pairsOf flattens the resolver output into "src>dst" strings for comparison.
func pairsOf(sources []registry.Node, destsFor map[string][]registry.Node) []string {
	var out []string
	for _, s := range sources {
		for _, d := range destsFor[s.InstanceID] {
			out = append(out, s.InstanceID+">"+d.InstanceID)
		}
	}
	sort.Strings(out)
	return out
}

func TestResolvePairsScopes(t *testing.T) {
	four := fleet(4) // i-1 i-2 i-3 i-4

	cases := []struct {
		name      string
		ids       []string
		scope     string
		wantPairs []string
		wantErr   bool
		// wantSkipped are ids the resolver must report as dropped.
		wantSkipped []string
	}{
		{
			// Empty target set must reproduce today's full NxN exactly.
			name:  "empty ids is full mesh",
			ids:   nil,
			scope: "among",
			wantPairs: []string{
				"i-1>i-2", "i-1>i-3", "i-1>i-4",
				"i-2>i-1", "i-2>i-3", "i-2>i-4",
				"i-3>i-1", "i-3>i-2", "i-3>i-4",
				"i-4>i-1", "i-4>i-2", "i-4>i-3",
			},
		},
		{
			// Scope is ignored when no targets are given.
			name:  "empty ids ignores scope",
			ids:   nil,
			scope: "fanin",
			wantPairs: []string{
				"i-1>i-2", "i-1>i-3", "i-1>i-4",
				"i-2>i-1", "i-2>i-3", "i-2>i-4",
				"i-3>i-1", "i-3>i-2", "i-3>i-4",
				"i-4>i-1", "i-4>i-2", "i-4>i-3",
			},
		},
		{
			// among: k*(k-1) among the selected nodes only.
			name:      "among 3 selected",
			ids:       []string{"i-1", "i-2", "i-3"},
			scope:     "among",
			wantPairs: []string{"i-1>i-2", "i-1>i-3", "i-2>i-1", "i-2>i-3", "i-3>i-1", "i-3>i-2"},
		},
		{
			name:      "among 2 selected is one pair each way",
			ids:       []string{"i-2", "i-4"},
			scope:     "among",
			wantPairs: []string{"i-2>i-4", "i-4>i-2"},
		},
		{
			// fanout: selected sources reach every online node.
			name:      "fanout from 1 selected",
			ids:       []string{"i-1"},
			scope:     "fanout",
			wantPairs: []string{"i-1>i-2", "i-1>i-3", "i-1>i-4"},
		},
		{
			name:  "fanout from 2 selected",
			ids:   []string{"i-1", "i-2"},
			scope: "fanout",
			wantPairs: []string{
				"i-1>i-2", "i-1>i-3", "i-1>i-4",
				"i-2>i-1", "i-2>i-3", "i-2>i-4",
			},
		},
		{
			// fanin: every online node measures TO the selected nodes.
			name:      "fanin to 1 selected",
			ids:       []string{"i-3"},
			scope:     "fanin",
			wantPairs: []string{"i-1>i-3", "i-2>i-3", "i-4>i-3"},
		},
		{
			name:  "fanin to 2 selected",
			ids:   []string{"i-1", "i-2"},
			scope: "fanin",
			wantPairs: []string{
				"i-1>i-2", "i-2>i-1",
				"i-3>i-1", "i-3>i-2",
				"i-4>i-1", "i-4>i-2",
			},
		},
		{
			// Default scope when unspecified is among.
			name:      "empty scope defaults to among",
			ids:       []string{"i-1", "i-2"},
			scope:     "",
			wantPairs: []string{"i-1>i-2", "i-2>i-1"},
		},
		{
			// k=1 among yields 0 pairs: must be an error, not a silent no-op.
			name:    "among with 1 selected is an error",
			ids:     []string{"i-1"},
			scope:   "among",
			wantErr: true,
		},
		{
			// Unknown ids are dropped and reported.
			name:        "unknown ids are skipped",
			ids:         []string{"i-1", "i-2", "i-nope"},
			scope:       "among",
			wantPairs:   []string{"i-1>i-2", "i-2>i-1"},
			wantSkipped: []string{"i-nope"},
		},
		{
			// All ids unknown leaves nothing to run.
			name:        "all ids unknown is an error",
			ids:         []string{"i-nope", "i-alsono"},
			scope:       "among",
			wantErr:     true,
			wantSkipped: []string{"i-alsono", "i-nope"},
		},
		{
			name:    "unknown scope is an error",
			ids:     []string{"i-1", "i-2"},
			scope:   "sideways",
			wantErr: true,
		},
		{
			// Duplicate ids must not produce duplicate pairs.
			name:      "duplicate ids are de-duplicated",
			ids:       []string{"i-1", "i-2", "i-1"},
			scope:     "among",
			wantPairs: []string{"i-1>i-2", "i-2>i-1"},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			sources, destsFor, skipped, err := ResolvePairs(four, tc.ids, tc.scope)
			if tc.wantErr {
				if err == nil {
					t.Fatalf("expected an error, got pairs %v", pairsOf(sources, destsFor))
				}
				if len(tc.wantSkipped) > 0 {
					sort.Strings(skipped)
					if strings.Join(skipped, ",") != strings.Join(tc.wantSkipped, ",") {
						t.Fatalf("skipped = %v, want %v", skipped, tc.wantSkipped)
					}
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			got := pairsOf(sources, destsFor)
			if strings.Join(got, ",") != strings.Join(tc.wantPairs, ",") {
				t.Fatalf("pairs =\n  %v\nwant\n  %v", got, tc.wantPairs)
			}
			sort.Strings(skipped)
			if strings.Join(skipped, ",") != strings.Join(tc.wantSkipped, ",") {
				t.Fatalf("skipped = %v, want %v", skipped, tc.wantSkipped)
			}
			// No node may ever measure to itself.
			for _, p := range got {
				if h := strings.Split(p, ">"); h[0] == h[1] {
					t.Fatalf("self-pair produced: %s", p)
				}
			}
		})
	}
}

// Offline nodes must never be measured, whether they were explicitly targeted
// or would have been pulled in as destinations by fanout/fanin.
func TestResolvePairsSkipsOfflineNodes(t *testing.T) {
	nodes := fleet(4)
	nodes[2].Online = false // i-3 is offline

	t.Run("offline target is skipped and reported", func(t *testing.T) {
		sources, destsFor, skipped, err := ResolvePairs(nodes, []string{"i-1", "i-2", "i-3"}, "among")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		want := []string{"i-1>i-2", "i-2>i-1"}
		if got := pairsOf(sources, destsFor); strings.Join(got, ",") != strings.Join(want, ",") {
			t.Fatalf("pairs = %v, want %v", got, want)
		}
		if strings.Join(skipped, ",") != "i-3" {
			t.Fatalf("skipped = %v, want [i-3]", skipped)
		}
	})

	t.Run("offline node is not a fanout destination", func(t *testing.T) {
		sources, destsFor, _, err := ResolvePairs(nodes, []string{"i-1"}, "fanout")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		want := []string{"i-1>i-2", "i-1>i-4"}
		if got := pairsOf(sources, destsFor); strings.Join(got, ",") != strings.Join(want, ",") {
			t.Fatalf("pairs = %v, want %v", got, want)
		}
	})

	t.Run("offline node is not a fanin source", func(t *testing.T) {
		sources, destsFor, _, err := ResolvePairs(nodes, []string{"i-1"}, "fanin")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		want := []string{"i-2>i-1", "i-4>i-1"}
		if got := pairsOf(sources, destsFor); strings.Join(got, ",") != strings.Join(want, ",") {
			t.Fatalf("pairs = %v, want %v", got, want)
		}
	})

	t.Run("full mesh excludes offline nodes", func(t *testing.T) {
		sources, destsFor, _, err := ResolvePairs(nodes, nil, "among")
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		for _, p := range pairsOf(sources, destsFor) {
			if strings.Contains(p, "i-3") {
				t.Fatalf("offline node appeared in full mesh: %s", p)
			}
		}
	})
}

// The prepare phase converges hosts before measuring. It must cover the UNION of
// sources and destinations: a destination left in client profile by an earlier
// run will not echo, so a fanin/fanout run would measure against a dead peer.
func TestPrepareSetIsUnionOfSourcesAndDests(t *testing.T) {
	four := fleet(4)

	cases := []struct {
		name  string
		ids   []string
		scope string
		want  []string
	}{
		{"fanout covers targeted source and all dests", []string{"i-1"}, "fanout",
			[]string{"i-1", "i-2", "i-3", "i-4"}},
		{"fanin covers all sources and targeted dest", []string{"i-3"}, "fanin",
			[]string{"i-1", "i-2", "i-3", "i-4"}},
		{"among covers only the selected nodes", []string{"i-1", "i-2"}, "among",
			[]string{"i-1", "i-2"}},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			sources, destsFor, _, err := ResolvePairs(four, tc.ids, tc.scope)
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			got := PrepareSet(sources, destsFor)
			var ids []string
			for _, n := range got {
				ids = append(ids, n.InstanceID)
			}
			sort.Strings(ids)
			if strings.Join(ids, ",") != strings.Join(tc.want, ",") {
				t.Fatalf("prepare set = %v, want %v", ids, tc.want)
			}
			// Must be free of duplicates: each node is converged once.
			seen := map[string]bool{}
			for _, id := range ids {
				if seen[id] {
					t.Fatalf("duplicate in prepare set: %s", id)
				}
				seen[id] = true
			}
		})
	}
}

// The job event has to state what actually ran, since a scoped run's cell count
// no longer implies the fleet size.
func TestScopeDescription(t *testing.T) {
	cases := []struct {
		scope string
		k     int
		want  string
	}{
		{"", 0, "full mesh"},
		{"among", 0, "full mesh"},
		{"among", 3, "among 3 selected nodes"},
		{"fanout", 1, "1 selected node to all"},
		{"fanout", 2, "2 selected nodes to all"},
		{"fanin", 1, "all to 1 selected node"},
		{"fanin", 3, "all to 3 selected nodes"},
	}
	for _, tc := range cases {
		if got := ScopeDescription(tc.scope, tc.k); got != tc.want {
			t.Fatalf("ScopeDescription(%q, %d) = %q, want %q", tc.scope, tc.k, got, tc.want)
		}
	}
}

// countPairs is what the UI uses for its button label, so it must agree with the
// resolver exactly or the label lies about what will run.
func TestCountPairsMatchesResolver(t *testing.T) {
	six := fleet(6)
	for _, scope := range []string{"among", "fanout", "fanin"} {
		for k := 0; k <= 4; k++ {
			ids := []string{}
			for i := 1; i <= k; i++ {
				ids = append(ids, "i-"+string(rune('0'+i)))
			}
			sources, destsFor, _, err := ResolvePairs(six, ids, scope)
			want := 0
			if err == nil {
				want = len(pairsOf(sources, destsFor))
			}
			if got := CountPairs(len(six), k, scope); got != want {
				t.Fatalf("CountPairs(N=6, k=%d, %s) = %d, resolver produced %d", k, scope, got, want)
			}
		}
	}
}

// The web UI posts nodes/scope in the run body, so the wire contract must bind
// onto the params struct without any explicit handling in handleRun.
func TestScopedRunJSONBinding(t *testing.T) {
	// UcastMatrixParams is in the orchestrator package, so test the JSON binding
	// on the fields that matter to pairs: nodes and scope.
	type runReq struct {
		Nodes []string `json:"nodes,omitempty"`
		Scope string   `json:"scope,omitempty"`
		Count int      `json:"count"`
	}
	body := `{"nodes":["i-abc","i-def"],"scope":"fanout","count":5000}`
	var p runReq
	if err := json.Unmarshal([]byte(body), &p); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if len(p.Nodes) != 2 || p.Nodes[0] != "i-abc" || p.Nodes[1] != "i-def" {
		t.Fatalf("nodes = %v, want [i-abc i-def]", p.Nodes)
	}
	if p.Scope != "fanout" {
		t.Fatalf("scope = %q, want fanout", p.Scope)
	}
	if p.Count != 5000 {
		t.Fatalf("existing fields broken: %+v", p)
	}

	// Omitting both must leave them zero so the resolver picks full mesh: this
	// is what keeps old clients on the pre-existing NxN behaviour.
	var q runReq
	if err := json.Unmarshal([]byte(`{}`), &q); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if len(q.Nodes) != 0 || q.Scope != "" {
		t.Fatalf("absent fields should stay zero, got nodes=%v scope=%q", q.Nodes, q.Scope)
	}
	sources, destsFor, _, err := ResolvePairs(fleet(3), q.Nodes, q.Scope)
	if err != nil {
		t.Fatalf("full mesh must resolve: %v", err)
	}
	if got := len(pairsOf(sources, destsFor)); got != 6 {
		t.Fatalf("full mesh over 3 nodes = %d pairs, want 6", got)
	}
}
