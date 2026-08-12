// Command afxdpctl is a single, typed entrypoint for the AF_XDP benchmark dev
// loop. It replaces ad-hoc bash + nested ansible with one tool that:
//
//   - drives infra via the CDK CLI      (up / down)
//   - hot-deploys code via ansible       (sync)
//   - talks to the control-plane HTTP/SSE API for the measurement loop
//     (fleet / run / cancel / report)
//
// The control-plane URL comes from -cp or $CP_URL (default http://localhost:8080).
//
// Examples:
//   afxdpctl fleet
//   afxdpctl run ucast kernel
//   afxdpctl run mcast copy,inplace,kernel
//   afxdpctl cancel
//   afxdpctl report -o run.html
//   afxdpctl up   --key virginia --scenario ucast/az-cpg-3 --git-repo <url> --git-ref <branch> [--bake]
//   afxdpctl sync --key ~/.ssh/virginia.pem --region us-east-1 [--profile P]
//   afxdpctl down --key virginia [--scenario ucast/az-cpg-3]
package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"sort"
	"strings"
	"time"
)

func cpURL() string {
	if v := os.Getenv("CP_URL"); v != "" {
		return v
	}
	return "http://localhost:8080"
}

// ── API types (subset of the backend JSON) ───────────────────────────────────

type pct struct {
	P50, P90, P99, P999, Max int64
}
type metrics struct {
	ServiceRTT pct     `json:"service_rtt_us"`
	LossPct    float64 `json:"loss_pct"`
}
type node struct {
	Role           string `json:"role"`
	PrivateIP      string `json:"private_ip"`
	PublicIP       string `json:"public_ip"`
	Online         bool   `json:"online"`
	ReplicatorMode string `json:"replicator_mode"`
}
type edge struct {
	Src, Dst, Kind, Variation string
	Unix                      int64
	Metrics                   metrics
}
type fleetResp struct {
	Nodes []node `json:"nodes"`
	Edges []edge `json:"edges"`
}

func getFleet(base string) (*fleetResp, error) {
	resp, err := http.Get(base + "/api/fleet")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("GET /api/fleet -> %s", resp.Status)
	}
	var f fleetResp
	if err := json.NewDecoder(resp.Body).Decode(&f); err != nil {
		return nil, err
	}
	return &f, nil
}

func postJSON(base, path string, body any) error {
	b, _ := json.Marshal(body)
	resp, err := http.Post(base+path, "application/json", bytes.NewReader(b))
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 300 {
		return fmt.Errorf("POST %s -> %s", path, resp.Status)
	}
	return nil
}

// fmtJob renders a backend job event for the console log.
func fmtJob(d map[string]any) string {
	s, _ := d["status"].(string)
	kind, _ := d["kind"].(string)
	kv := kind
	if v, ok := d["variation"].(string); ok && v != "" {
		kv += "/" + v
	} else if m, ok := d["mode"].(string); ok && m != "" {
		kv += "/" + m
	}
	if msg, ok := d["msg"].(string); ok && msg != "" {
		return fmt.Sprintf("%s: %s", kv, msg)
	}
	switch s {
	case "progress":
		if src, ok := d["src"].(string); ok {
			return fmt.Sprintf("%s: %v/%v %s->%v", kv, num(d["done"]), num(d["total"]), src, d["dst"])
		}
	case "done":
		return fmt.Sprintf("done %s", kv)
	case "cancelled":
		return fmt.Sprintf("cancelled %s", kv)
	case "rejected":
		return fmt.Sprintf("rejected: %v", d["reason"])
	case "error":
		return fmt.Sprintf("error %s: %v%v", kv, d["reason"], d["err"])
	}
	return fmt.Sprintf("%s %s", s, kv)
}
func num(v any) int64 {
	if f, ok := v.(float64); ok {
		return int64(f)
	}
	return 0
}

// streamUntilTerminal reads the SSE job log, printing each job event, and
// returns once a terminal status (done/cancelled/error/rejected) arrives.
func streamUntilTerminal(base string, timeout time.Duration) {
	req, _ := http.NewRequest("GET", base+"/api/events", nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		fmt.Println("event stream:", err)
		return
	}
	defer resp.Body.Close()
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	deadline := time.Now().Add(timeout)
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "data: ") {
			if time.Now().After(deadline) {
				return
			}
			continue
		}
		var ev struct {
			Type string         `json:"type"`
			Data map[string]any `json:"data"`
		}
		if json.Unmarshal([]byte(strings.TrimPrefix(line, "data: ")), &ev) != nil {
			continue
		}
		if ev.Type != "job" {
			continue
		}
		fmt.Printf("  %s\n", fmtJob(ev.Data))
		switch ev.Data["status"] {
		case "done", "cancelled", "error", "rejected":
			return
		}
		if time.Now().After(deadline) {
			return
		}
	}
}

// ── commands ──────────────────────────────────────────────────────────────

func cmdFleet(base string) error {
	f, err := getFleet(base)
	if err != nil {
		return err
	}
	online := 0
	for _, n := range f.Nodes {
		if n.Online {
			online++
		}
	}
	fmt.Printf("fleet @ %s — %d/%d online, %d edges\n", base, online, len(f.Nodes), len(f.Edges))
	fmt.Printf("  %-13s %-16s %-16s %-6s %s\n", "ROLE", "PRIVATE", "PUBLIC", "STATE", "REPL")
	for _, n := range f.Nodes {
		st := "off"
		if n.Online {
			st = "on"
		}
		fmt.Printf("  %-13s %-16s %-16s %-6s %s\n", n.Role, n.PrivateIP, n.PublicIP, st, n.ReplicatorMode)
	}
	return nil
}

func printMatrix(base, kind, variation string) {
	f, err := getFleet(base)
	if err != nil {
		return
	}
	fmt.Printf("\n%s/%s edges:\n", kind, variation)
	var es []edge
	for _, e := range f.Edges {
		if e.Kind == kind && (variation == "" || variation == "all" || e.Variation == variation) {
			es = append(es, e)
		}
	}
	sort.Slice(es, func(i, j int) bool {
		if es[i].Src != es[j].Src {
			return es[i].Src < es[j].Src
		}
		return es[i].Dst < es[j].Dst
	})
	for _, e := range es {
		m := e.Metrics.ServiceRTT
		fmt.Printf("  %-16s -> %-16s %s p50=%dus p99=%dus loss=%.2f%%\n", e.Src, e.Dst, e.Variation, m.P50, m.P99, e.Metrics.LossPct)
	}
	if len(es) == 0 {
		fmt.Println("  (none)")
	}
}

func cmdRun(base string, args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: run ucast <variation> | run mcast <modes,csv>")
	}
	kind := args[0]
	var body map[string]any
	variation := ""
	switch kind {
	case "ucast":
		variation = "kernel"
		if len(args) > 1 {
			variation = args[1]
		}
		body = map[string]any{"kind": "ucast", "variation": variation, "count": 5000, "rate": 20000, "warmup": 1000}
	case "mcast":
		modes := []string{"copy"}
		if len(args) > 1 {
			modes = strings.Split(args[1], ",")
		}
		variation = modes[0]
		body = map[string]any{"kind": "mcast", "modes": modes, "count": 5000, "interval_us": 100, "timeout_sec": 25}
	default:
		return fmt.Errorf("kind must be ucast or mcast")
	}
	fmt.Printf("launching %s %v ...\n", kind, args[1:])
	if err := postJSON(base, "/api/run", body); err != nil {
		return err
	}
	streamUntilTerminal(base, 10*time.Minute)
	printMatrix(base, kind, variation)
	return nil
}

func cmdReport(base string, args []string) error {
	fs := flag.NewFlagSet("report", flag.ExitOnError)
	out := fs.String("o", "afxdp-report.html", "output HTML file")
	kind := fs.String("kind", "", "filter kind (ucast|mcast; default: all edges)")
	fs.Parse(args)
	f, err := getFleet(base)
	if err != nil {
		return err
	}
	html := buildReport(f, *kind)
	if err := os.WriteFile(*out, []byte(html), 0o644); err != nil {
		return err
	}
	fmt.Printf("wrote %s (%d nodes, %d edges)\n", *out, len(f.Nodes), len(f.Edges))
	return nil
}

// ── infra wrappers (cdk / ansible) ──────────────────────────────────────────

func run(dir, name string, args ...string) error {
	c := exec.Command(name, args...)
	c.Dir = dir
	c.Stdout, c.Stderr, c.Stdin = os.Stdout, os.Stderr, os.Stdin
	fmt.Printf("+ (cd %s && %s %s)\n", dir, name, strings.Join(args, " "))
	return c.Run()
}

func cmdUp(args []string) error {
	fs := flag.NewFlagSet("up", flag.ExitOnError)
	key := fs.String("key", "", "EC2 key pair name (required)")
	scenario := fs.String("scenario", "ucast/az-cpg-3", "fleet scenario")
	repo := fs.String("git-repo", "", "git repo for control-plane + AMI bake")
	ref := fs.String("git-ref", "main", "git ref")
	bake := fs.Bool("bake", false, "also (re)bake the AMI")
	instType := fs.String("instance-type", "c7i.4xlarge", "AMI builder instance type")
	cdkDir := fs.String("cdk-dir", "deploy/cdk", "path to the CDK app")
	fs.Parse(args)
	if *key == "" || *repo == "" {
		return fmt.Errorf("up requires --key and --git-repo")
	}
	ctx := []string{"--require-approval", "never", "--context", "keyPairName=" + *key, "--context", "gitRepo=" + *repo, "--context", "gitRef=" + *ref}
	if err := run(*cdkDir, "npx", append([]string{"cdk", "deploy", "XdpStack-ControlPlane", "--context", "deploymentType=control-plane"}, ctx...)...); err != nil {
		return err
	}
	if *bake {
		if err := run(*cdkDir, "npx", append([]string{"cdk", "deploy", "XdpStack-AmiBuilder", "--context", "deploymentType=ami-builder", "--context", "instanceType=" + *instType}, ctx...)...); err != nil {
			return err
		}
	}
	return run(*cdkDir, "npx", "cdk", "deploy", "XdpStack", "--require-approval", "never", "--context", "keyPairName="+*key, "--context", "scenario="+*scenario)
}

func cmdSync(args []string) error {
	fs := flag.NewFlagSet("sync", flag.ExitOnError)
	key := fs.String("key", os.Getenv("SSH_KEY_FILE"), "SSH key file (or $SSH_KEY_FILE)")
	region := fs.String("region", "us-east-1", "AWS region")
	profile := fs.String("profile", os.Getenv("AWS_PROFILE"), "AWS profile")
	dir := fs.String("ansible-dir", "dev/ansible", "path to the dev ansible dir")
	fs.Parse(args)
	if *key == "" {
		return fmt.Errorf("sync requires --key (or $SSH_KEY_FILE)")
	}
	c := exec.Command("ansible-playbook", "-i", "inventory.aws_ec2.yml", "sync.yaml")
	c.Dir = *dir
	c.Stdout, c.Stderr = os.Stdout, os.Stderr
	c.Env = append(os.Environ(), "SSH_KEY_FILE="+*key, "AWS_DEFAULT_REGION="+*region, "ANSIBLE_HOST_KEY_CHECKING=False")
	if *profile != "" {
		c.Env = append(c.Env, "AWS_PROFILE="+*profile)
	}
	fmt.Printf("+ (cd %s && ansible-playbook sync.yaml)\n", *dir)
	return c.Run()
}

func cmdDown(args []string) error {
	fs := flag.NewFlagSet("down", flag.ExitOnError)
	key := fs.String("key", "x", "EC2 key pair name (context only)")
	scenario := fs.String("scenario", "ucast/az-cpg-3", "scenario context (for fleet synth)")
	cdkDir := fs.String("cdk-dir", "deploy/cdk", "path to the CDK app")
	fs.Parse(args)
	// Destroy in reverse dependency order; each is best-effort.
	_ = run(*cdkDir, "npx", "cdk", "destroy", "--force", "XdpStack", "--context", "keyPairName="+*key, "--context", "scenario="+*scenario)
	_ = run(*cdkDir, "npx", "cdk", "destroy", "--force", "XdpStack-ControlPlane", "--context", "deploymentType=control-plane", "--context", "keyPairName="+*key)
	_ = run(*cdkDir, "npx", "cdk", "destroy", "--force", "XdpStack-AmiBuilder", "--context", "deploymentType=ami-builder", "--context", "keyPairName="+*key)
	return nil
}

func usage() {
	fmt.Print(`afxdpctl — AF_XDP benchmark control CLI

  Measurement (talks to the control-plane API; -cp or $CP_URL):
    fleet                     show nodes + edge count
    run ucast <variation>     kernel|xdp-tx|xdp-rx|xdp-txrx|all
    run mcast <modes,csv>     copy,inplace,kernel
    cancel                    abort the running campaign
    report [-o file] [-kind]  write an HTML report (heatmap + all latencies)

  Infra (wrap CDK / ansible):
    up   --key K --git-repo R [--git-ref B] [--scenario S] [--bake]
    sync --key KEYFILE [--region R] [--profile P]
    down --key K [--scenario S]

  Global: -cp <url>   control-plane base URL (default $CP_URL or http://localhost:8080)
`)
}

func main() {
	cp := flag.String("cp", cpURL(), "control-plane base URL")
	flag.Usage = usage
	flag.Parse()
	args := flag.Args()
	if len(args) == 0 {
		usage()
		os.Exit(2)
	}
	var err error
	switch args[0] {
	case "fleet":
		err = cmdFleet(*cp)
	case "run":
		err = cmdRun(*cp, args[1:])
	case "cancel":
		if err = postJSON(*cp, "/api/cancel", map[string]any{}); err == nil {
			fmt.Println("cancel requested")
		}
	case "report":
		err = cmdReport(*cp, args[1:])
	case "up":
		err = cmdUp(args[1:])
	case "sync":
		err = cmdSync(args[1:])
	case "down":
		err = cmdDown(args[1:])
	case "help", "-h", "--help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n", args[0])
		usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}
