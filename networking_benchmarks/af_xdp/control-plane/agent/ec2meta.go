package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"os/exec"
	"strings"
	"time"

	"afxdp-cp/proto"
)

// enrichFromEC2 calls `aws ec2 describe-instance-types` via the CLI (which
// handles SigV4 signing using the instance's IAM role credentials from IMDS).
// Best-effort: failures are logged but don't block agent startup.
func enrichFromEC2(n *proto.NodeInfo) {
	if n.InstanceType == "" || n.Region == "" {
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "aws", "ec2", "describe-instance-types",
		"--instance-types", n.InstanceType,
		"--region", n.Region,
		"--output", "json",
	)
	out, err := cmd.Output()
	if err != nil {
		log.Printf("ec2meta: describe-instance-types failed: %v", err)
		return
	}

	var resp struct {
		InstanceTypes []struct {
			VCPUInfo struct {
				DefaultVCpus int `json:"DefaultVCpus"`
			} `json:"VCpuInfo"`
			MemoryInfo struct {
				SizeInMiB int `json:"SizeInMiB"`
			} `json:"MemoryInfo"`
			NetworkInfo struct {
				NetworkPerformance       string `json:"NetworkPerformance"`
				MaximumNetworkInterfaces int    `json:"MaximumNetworkInterfaces"`
			} `json:"NetworkInfo"`
			BareMetal bool `json:"BareMetal"`
		} `json:"InstanceTypes"`
	}
	if err := json.Unmarshal(out, &resp); err != nil {
		log.Printf("ec2meta: parse failed: %v", err)
		return
	}
	if len(resp.InstanceTypes) == 0 {
		log.Printf("ec2meta: no results for %s", n.InstanceType)
		return
	}

	it := resp.InstanceTypes[0]
	n.VCPUs = it.VCPUInfo.DefaultVCpus
	n.MemGB = float64(it.MemoryInfo.SizeInMiB) / 1024.0
	n.ENIs = it.NetworkInfo.MaximumNetworkInterfaces
	n.Metal = it.BareMetal
	n.BwGbps = parseGbps(it.NetworkInfo.NetworkPerformance)
	n.NitroGen = inferNitroGen(n.InstanceType)

	log.Printf("ec2meta: %s → %dvCPU, %.0fGB, %.0fGbps, %dENIs",
		n.InstanceType, n.VCPUs, n.MemGB, n.BwGbps, n.ENIs)
}

func parseGbps(perf string) float64 {
	perf = strings.Replace(perf, "Up to ", "", 1)
	perf = strings.Replace(perf, " Gigabit", "", 1)
	var gbps float64
	fmt.Sscanf(perf, "%f", &gbps)
	return gbps
}

func inferNitroGen(instType string) string {
	parts := strings.SplitN(instType, ".", 2)
	if len(parts) == 0 {
		return ""
	}
	family := parts[0]
	for i, ch := range family {
		if ch >= '0' && ch <= '9' {
			return "gen" + family[i:i+1]
		}
	}
	return ""
}
