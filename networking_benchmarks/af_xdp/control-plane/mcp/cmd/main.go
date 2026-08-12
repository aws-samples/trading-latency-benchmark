package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"log"
	"os"

	"afxdp-cp/mcp"
)

func main() {
	dbPath := os.Getenv("MCP_DB_PATH")
	if dbPath == "" {
		// Also accept as a CLI argument for convenience.
		if len(os.Args) > 1 {
			dbPath = os.Args[1]
		}
	}
	if dbPath == "" {
		fmt.Fprintf(os.Stderr, "usage: set MCP_DB_PATH or pass db path as argument\n")
		os.Exit(1)
	}

	db, err := mcp.OpenDB(dbPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	defer db.Close()

	srv := mcp.NewServer(db)
	log.SetOutput(os.Stderr)

	scanner := bufio.NewScanner(os.Stdin)
	// MCP uses newline-delimited JSON-RPC.
	scanner.Buffer(make([]byte, 0, 1024*1024), 1024*1024)

	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}

		var req mcp.JSONRPCRequest
		if err := json.Unmarshal(line, &req); err != nil {
			resp := mcp.JSONRPCResponse{
				JSONRPC: "2.0",
				Error:   &mcp.JSONRPCError{Code: -32700, Message: "parse error: " + err.Error()},
			}
			writeResponse(resp)
			continue
		}

		resp := srv.Handle(req)
		// Notifications (no ID) don't get a response per JSON-RPC spec.
		if req.ID == nil && req.Method == "notifications/initialized" {
			continue
		}
		writeResponse(resp)
	}
}

func writeResponse(resp mcp.JSONRPCResponse) {
	out, _ := json.Marshal(resp)
	os.Stdout.Write(out)
	os.Stdout.Write([]byte("\n"))
}
