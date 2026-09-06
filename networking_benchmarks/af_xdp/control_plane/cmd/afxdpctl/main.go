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
//   afxdpctl run ucast kernel -count 50000 -rate 10000
//   afxdpctl run mcast copy,inplace,xdp_tx -count 10000 -interval-us 200
//   afxdpctl cancel
//   afxdpctl report -o run.html
//   afxdpctl up   --key frankfurt --secondary-key london --scenario all --git-repo <url> --git-ref <branch> --bake
//   afxdpctl sync --key ~/.ssh/frankfurt.pem --region eu-central-1
//   afxdpctl down --key frankfurt --scenario all
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
	AZ             string `json:"az"`
	Region         string `json:"region"`
	InstanceType   string `json:"instance_type"`
	Tenancy        string `json:"tenancy"`
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
		return fmt.Errorf("usage: run ucast <variation> [-count N] [-rate R] [-warmup W] [-max-parallel P] [-max-loss PCT] [-xdp-tx] [-xdp-rx] [-xdp-tx-queue Q] [-send-cpu C] [-recv-cpu C]\n       run mcast <modes,csv> [-count N] [-interval-us I] [-timeout T] [-size B] [-group G] [-data-port P] [-tx-queue Q] [-rx-queue Q]")
	}
	kind := args[0]
	var body map[string]any
	variation := ""
	switch kind {
	case "ucast":
		fs := flag.NewFlagSet("run-ucast", flag.ExitOnError)
		count := fs.Int("count", 10000, "messages per pair")
		rate := fs.Int("rate", 10000, "messages/sec")
		warmup := fs.Int("warmup", 1000, "warmup messages")
		maxPar := fs.Int("max-parallel", 4, "max concurrent pairs per round (1=serial, 0=unlimited)")
		maxLoss := fs.Float64("max-loss", 2.0, "reject a pair whose loss exceeds this % (percentiles from a lossy run are survivorship-biased); -1 disables")
		// xdpTx/xdpRx default to the variation-derived behavior (nil => let the
		// backend derive both from variation=="xdp"); passing either flag
		// overrides just that leg, matching run_ucast.yaml's independent
		// xdp_tx/xdp_rx vars.
		xdpTx := fs.Bool("xdp-tx", false, "force AF_XDP client TX on, independent of variation")
		xdpRx := fs.Bool("xdp-rx", false, "force AF_XDP client RX on, independent of variation")
		xdpTxQueue := fs.Int("xdp-tx-queue", 0, "AF_XDP TX queue when xdp-tx is active (0 = backend default, queue 1)")
		sendCPU := fs.Int("send-cpu", 0, "pin the rtt TX thread to this CPU (0 = auto-derive from isolated set)")
		recvCPU := fs.Int("recv-cpu", 0, "pin the rtt RX thread to this CPU (0 = auto-derive from isolated set)")
		// The variation ("kernel"/"xdp") is a positional arg that comes
		// BEFORE the flags (`run ucast kernel -count N`), but Go's flag
		// package stops parsing at the first non-flag token - if the
		// variation were left in args[1:], fs.Parse would halt on it
		// immediately and every flag after it would be silently ignored
		// (count/rate/warmup/max-parallel/max-loss would all stick at their
		// defaults with no error). Strip the variation token out before
		// parsing, then feed the flag package only the actual flags,
		// wherever they are.
		variation = "kernel"
		flagArgs := args[1:]
		if len(flagArgs) > 0 && !strings.HasPrefix(flagArgs[0], "-") {
			variation = flagArgs[0]
			flagArgs = flagArgs[1:]
		}
		fs.Parse(flagArgs)
		body = map[string]any{"kind": "ucast", "variation": variation, "count": *count, "rate": *rate, "warmup": *warmup, "max_parallel": *maxPar, "max_loss_pct": *maxLoss}
		// Only send xdp_tx/xdp_rx overrides when the operator actually passed
		// them - omitting the keys lets the backend's variation-derived
		// default apply, same as before these flags existed.
		fs.Visit(func(f *flag.Flag) {
			switch f.Name {
			case "xdp-tx":
				body["xdp_tx"] = *xdpTx
			case "xdp-rx":
				body["xdp_rx"] = *xdpRx
			case "xdp-tx-queue":
				body["xdp_tx_queue"] = *xdpTxQueue
			case "send-cpu":
				body["send_cpu"] = *sendCPU
			case "recv-cpu":
				body["recv_cpu"] = *recvCPU
			}
		})
	case "mcast":
		fs := flag.NewFlagSet("run-mcast", flag.ExitOnError)
		count := fs.Int("count", 10000, "messages")
		intervalUs := fs.Int("interval-us", 200, "inter-message interval (µs)")
		timeout := fs.Int("timeout", 30, "receive timeout (sec)")
		size := fs.Int("size", 0, "mcast_send payload bytes (0 = tool default, 64B; tool minimum 32B)")
		group := fs.String("group", "", "multicast group tag (0 = tool default, 224.0.31.50)")
		dataPort := fs.Int("data-port", 0, "UDP data port (0 = tool default, 5000)")
		txQueue := fs.Int("tx-queue", 0, "mcast_send AF_XDP TX queue (0 = tool default, queue 1)")
		rxQueue := fs.Int("rx-queue", 0, "mcast_receive AF_XDP/XDP queue index (0 = tool default, queue 0)")
		// The modes CSV is a positional arg that comes BEFORE the flags
		// (`run mcast copy,inplace,xdp_tx -count N`), but Go's flag package
		// stops parsing at the first non-flag token - if the modes CSV were
		// left in args[1:], fs.Parse would halt on it immediately and every
		// flag after it would be silently ignored (count/interval-us/timeout
		// would all stick at their defaults with no error). Strip the modes
		// token out before parsing, then feed the flag package only the
		// actual flags, wherever they are.
		modesArg := "copy"
		flagArgs := args[1:]
		if len(flagArgs) > 0 && !strings.HasPrefix(flagArgs[0], "-") {
			modesArg = flagArgs[0]
			flagArgs = flagArgs[1:]
		}
		fs.Parse(flagArgs)
		modes := strings.Split(modesArg, ",")
		variation = modes[0]
		body = map[string]any{"kind": "mcast", "modes": modes, "count": *count, "interval_us": *intervalUs, "timeout_sec": *timeout,
			"size": *size, "tx_queue": *txQueue, "rx_queue": *rxQueue}
		if *group != "" {
			body["group"] = *group
		}
		if *dataPort != 0 {
			body["data_port"] = *dataPort
		}
	default:
		return fmt.Errorf("kind must be ucast or mcast")
	}
	fmt.Printf("launching %s/%s ...\n", kind, variation)
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
	key := fs.String("key", "", "EC2 key pair name for primary region (required)")
	secondaryKey := fs.String("secondary-key", "", "EC2 key pair name for secondary region (cross-region deploys)")
	scenario := fs.String("scenario", "ucast-cpg-3", "fleet scenario")
	repo := fs.String("git-repo", "", "git repo for control-plane + AMI bake")
	ref := fs.String("git-ref", "main", "git ref")
	bake := fs.Bool("bake", false, "also (re)bake the AMI")
	instType := fs.String("instance-type", "m8a.2xlarge", "AMI builder instance type")
	cdkDir := fs.String("cdk-dir", "deploy/cdk", "path to the CDK app")
	region := fs.String("region", "eu-central-1", "primary AWS region")
	fs.Parse(args)
	if *key == "" || *repo == "" {
		return fmt.Errorf("up requires --key and --git-repo")
	}
	ctx := []string{"--require-approval", "never",
		"--context", "keyPairName=" + *key,
		"--context", "gitRepo=" + *repo,
		"--context", "gitRef=" + *ref,
		"--context", "region=" + *region}
	if *secondaryKey != "" {
		ctx = append(ctx, "--context", "secondaryKeyPairName="+*secondaryKey)
	}
	if err := run(*cdkDir, "npx", append([]string{"cdk", "deploy", "XdpStack-ControlPlane", "--context", "deploymentType=control-plane"}, ctx...)...); err != nil {
		return err
	}
	if *bake {
		if err := run(*cdkDir, "npx", append([]string{"cdk", "deploy", "XdpStack-AmiBuilder", "--context", "deploymentType=ami-builder", "--context", "instanceType=" + *instType}, ctx...)...); err != nil {
			return err
		}
	}
	deployCtx := append(ctx, "--context", "scenario="+*scenario)
	return run(*cdkDir, "npx", append([]string{"cdk", "deploy", "--all"}, deployCtx...)...)
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
	// ANSIBLE_PRIVATE_KEY_FILE is set alongside SSH_KEY_FILE (not instead of
	// it): inventory.aws_ec2.yml's compose still documents/relies on
	// SSH_KEY_FILE via lookup('env', ...), but that lookup silently resolves
	// to empty inside an inventory plugin's compose context on ansible-core
	// >= 2.19-ish (no error - ansible_ssh_private_key_file is just absent from
	// the resulting hostvars), which left every SSH connection attempt with NO
	// identity file at all and a bare "Permission denied (publickey)" that
	// looked like a wrong/missing key rather than a templating gap.
	// ANSIBLE_PRIVATE_KEY_FILE is a core Ansible env var read directly by the
	// connection plugin, independent of inventory templating, so it's the
	// robust fix regardless of what inventory feature works or breaks next.
	c.Env = append(os.Environ(), "SSH_KEY_FILE="+*key, "ANSIBLE_PRIVATE_KEY_FILE="+*key,
		"AWS_DEFAULT_REGION="+*region, "ANSIBLE_HOST_KEY_CHECKING=False")
	if *profile != "" {
		c.Env = append(c.Env, "AWS_PROFILE="+*profile)
	}
	fmt.Printf("+ (cd %s && ansible-playbook sync.yaml)\n", *dir)
	return c.Run()
}

func cmdDown(args []string) error {
	fs := flag.NewFlagSet("down", flag.ExitOnError)
	key := fs.String("key", "x", "EC2 key pair name (context only)")
	scenario := fs.String("scenario", "ucast-cpg-3", "scenario context (for fleet synth)")
	cdkDir := fs.String("cdk-dir", "deploy/cdk", "path to the CDK app")
	region := fs.String("region", "eu-central-1", "primary AWS region")
	fs.Parse(args)
	ctx := []string{"--context", "keyPairName=" + *key, "--context", "region=" + *region}
	// Destroy in reverse dependency order; each is best-effort.
	_ = run(*cdkDir, "npx", append([]string{"cdk", "destroy", "--force", "--all", "--context", "scenario=" + *scenario}, ctx...)...)
	_ = run(*cdkDir, "npx", append([]string{"cdk", "destroy", "--force", "XdpStack-ControlPlane", "--context", "deploymentType=control-plane"}, ctx...)...)
	_ = run(*cdkDir, "npx", append([]string{"cdk", "destroy", "--force", "XdpStack-AmiBuilder", "--context", "deploymentType=ami-builder"}, ctx...)...)
	return nil
}

func usage() {
	fmt.Print(`afxdpctl — AF_XDP benchmark control CLI

  Measurement (talks to the control-plane API; -cp or $CP_URL):
    fleet                          show nodes + edge count
    run ucast [variation] [-count N] [-rate R] [-warmup W] [-max-parallel P] [-max-loss PCT]
                                   variation: kernel|xdp|all
    run mcast [modes,csv] [-count N] [-interval-us I] [-timeout T]
                                   modes: copy,inplace,xdp_tx
    cancel                         abort the running campaign
    report [-o file] [-kind]       write an HTML report (heatmap + all latencies)

  Infra (wrap CDK / ansible):
    up   --key K --git-repo R [--secondary-key K2] [--git-ref B] [--scenario S] [--region R] [--bake]
    sync --key KEYFILE [--region R] [--profile P]
    down --key K [--scenario S] [--region R]

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
