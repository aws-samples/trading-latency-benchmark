// Command afxdp-backend is the central control plane: it ingests agent
// registrations/heartbeats/telemetry over NATS into an authoritative in-memory
// fleet + NxN matrix, orchestrates campaigns, and serves a JSON+SSE API (and the
// static web app) to the browser. Designed to run on a small dedicated EC2
// alongside a NATS server (see the ControlPlaneStack CDK).
package main

import (
	"crypto/tls"
	"flag"
	"log"
	"net/http"
	"os"
	"time"

	"github.com/nats-io/nats.go"
)

func main() {
	natsURL := flag.String("nats", envOr("CP_NATS_URL", nats.DefaultURL), "NATS server URL")
	natsToken := flag.String("nats-token", envOr("CP_NATS_TOKEN", ""), "NATS auth token")
	natsInsecure := flag.Bool("nats-insecure", os.Getenv("CP_NATS_INSECURE") != "", "skip TLS verify (self-signed NATS)")
	addr := flag.String("addr", envOr("CP_HTTP_ADDR", ":8080"), "HTTP listen address")
	webDir := flag.String("web", envOr("CP_WEB_DIR", ""), "static web dir (default: auto-detect web/dist)")
	staleSec := flag.Int64("stale", 20, "seconds without a heartbeat before a node is marked offline")
	flag.Parse()

	natsOpts := []nats.Option{
		nats.Name("afxdp-backend"),
		nats.MaxReconnects(-1),
		nats.ReconnectWait(2 * time.Second),
	}
	if *natsToken != "" {
		natsOpts = append(natsOpts, nats.Token(*natsToken))
	}
	if *natsInsecure {
		natsOpts = append(natsOpts, nats.Secure(&tls.Config{InsecureSkipVerify: true}))
	}
	nc, err := nats.Connect(*natsURL, natsOpts...)
	if err != nil {
		log.Fatalf("nats connect %s: %v", *natsURL, err)
	}
	defer nc.Drain()

	reg := NewRegistry(*staleSec)
	coll := NewCollector()
	hub := NewHub()
	if err := startIngest(nc, reg, coll, hub); err != nil {
		log.Fatalf("ingest: %v", err)
	}
	orch, err := NewOrchestrator(nc, reg, hub)
	if err != nil {
		log.Fatalf("orchestrator: %v", err)
	}

	web := *webDir
	if web == "" {
		web = webDirDefault()
	}
	srv := &Server{reg: reg, coll: coll, hub: hub, orch: orch, web: web}
	log.Printf("backend up: nats=%s http=%s web=%q", *natsURL, *addr, web)
	if err := http.ListenAndServe(*addr, srv.routes()); err != nil {
		log.Fatalf("http: %v", err)
	}
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
