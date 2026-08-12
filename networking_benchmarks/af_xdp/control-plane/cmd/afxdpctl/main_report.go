package main

import (
	"fmt"
	"sort"
	"strings"
	"time"
)

// buildReport renders a self-contained HTML report (p50 heatmap + full latency
// table) from a fleet snapshot. If kind is "" all edges are included; otherwise
// only edges of that kind (ucast|mcast).
func buildReport(f *fleetResp, kind string) string {
	// Collect the ordered set of IPs that appear as src/dst in the kept edges,
	// and index edges by src|dst.
	type key struct{ s, d string }
	cells := map[key]edge{}
	ipset := map[string]bool{}
	var minP, maxP int64 = 1<<62, -1
	for _, e := range f.Edges {
		if kind != "" && e.Kind != kind {
			continue
		}
		cells[key{e.Src, e.Dst}] = e
		ipset[e.Src], ipset[e.Dst] = true, true
		p := e.Metrics.ServiceRTT.P50
		if p < minP {
			minP = p
		}
		if p > maxP {
			maxP = p
		}
	}
	if maxP < 0 {
		minP, maxP = 0, 1
	}
	ips := make([]string, 0, len(ipset))
	for ip := range ipset {
		ips = append(ips, ip)
	}
	sort.Strings(ips)

	// Heatmap.
	var b strings.Builder
	b.WriteString(`<table class="heat"><tr><th>src \ dst</th>`)
	for _, d := range ips {
		fmt.Fprintf(&b, "<th>%s</th>", d)
	}
	b.WriteString("</tr>")
	for _, s := range ips {
		fmt.Fprintf(&b, "<tr><th>%s</th>", s)
		for _, d := range ips {
			if s == d {
				b.WriteString(`<td class="diag">—</td>`)
				continue
			}
			e, ok := cells[key{s, d}]
			if !ok {
				b.WriteString(`<td class="na">·</td>`)
				continue
			}
			p := e.Metrics.ServiceRTT.P50
			fmt.Fprintf(&b, `<td style="background:%s" title="p99 %dus · loss %.2f%%">%dus</td>`,
				heatColor(p, minP, maxP), e.Metrics.ServiceRTT.P99, e.Metrics.LossPct, p)
		}
		b.WriteString("</tr>")
	}
	b.WriteString("</table>")

	// Latency table.
	var rows strings.Builder
	var es []edge
	for _, e := range cells {
		es = append(es, e)
	}
	sort.Slice(es, func(i, j int) bool {
		if es[i].Src != es[j].Src {
			return es[i].Src < es[j].Src
		}
		return es[i].Dst < es[j].Dst
	})
	for _, e := range es {
		m := e.Metrics.ServiceRTT
		fmt.Fprintf(&rows, "<tr><td>%s</td><td>%s</td><td>%s</td><td>%dus</td><td>%dus</td><td>%dus</td><td>%dus</td><td>%dus</td><td>%.2f%%</td></tr>",
			e.Src, e.Dst, e.Variation, m.P50, m.P90, m.P99, m.P999, m.Max, e.Metrics.LossPct)
	}

	title := kind
	if title == "" {
		title = "all"
	}
	return fmt.Sprintf(`<!doctype html><html><head><meta charset="utf-8"><title>AF_XDP report — %s</title>
<style>body{font:13px -apple-system,sans-serif;background:#0d1117;color:#e6edf3;padding:24px;margin:0}
h1{font-size:18px;margin:0 0 4px}h2{font-size:15px;margin:24px 0 4px;color:#58a6ff}.meta{color:#8b949e}
table{border-collapse:collapse;margin-top:8px}td,th{border:1px solid #30363d;padding:4px 9px;text-align:center;font-size:12px;font-family:'SF Mono',monospace}
th{background:#161b22;color:#8b949e}.heat td{color:#0d1117;font-weight:700}.heat .diag,.heat .na{background:#161b22;color:#6e7681;font-weight:400}</style></head><body>
<h1>AF_XDP latency report — %s</h1>
<div class="meta">Nodes: %d · Pairs: %d · Generated: %s</div>
<h2>Heatmap — p50 (green = fast, red = slow)</h2>%s
<h2>All measured latencies</h2>
<table><tr><th>src</th><th>dst</th><th>var</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th></tr>%s</table>
</body></html>`, title, title, len(f.Nodes), len(es), time.Now().Format(time.RFC3339), b.String(), rows.String())
}

// heatColor maps p50 in [lo,hi] to a green→orange→red CSS rgb() (matches the web).
func heatColor(p, lo, hi int64) string {
	t := 0.0
	if hi > lo {
		t = float64(p-lo) / float64(hi-lo)
	}
	if t < 0 {
		t = 0
	}
	if t > 1 {
		t = 1
	}
	stops := [][3]int{{57, 211, 83}, {240, 136, 62}, {248, 81, 73}}
	seg, lt := 0, t*2
	if t > 0.5 {
		seg, lt = 1, (t-0.5)*2
	}
	a, c := stops[seg], stops[seg+1]
	r := a[0] + int(float64(c[0]-a[0])*lt)
	g := a[1] + int(float64(c[1]-a[1])*lt)
	bl := a[2] + int(float64(c[2]-a[2])*lt)
	return fmt.Sprintf("rgb(%d,%d,%d)", r, g, bl)
}
