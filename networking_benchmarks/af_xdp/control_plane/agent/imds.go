package main

import (
	"io"
	"net/http"
	"os"
	"strings"
	"time"

	"afxdp-cp/proto"
)

const imdsBase = "http://169.254.169.254/latest"

// imdsClient fetches instance metadata via IMDSv2 (token-authenticated).
// All calls are best-effort with a short timeout so the agent still starts
// off-EC2 (falling back to env vars + hostname).
type imdsClient struct {
	hc    *http.Client
	token string
	skip  bool
}

func newIMDS() *imdsClient {
	c := &imdsClient{hc: &http.Client{Timeout: 2 * time.Second}}
	if strings.TrimSpace(os.Getenv("AGENT_NO_IMDS")) != "" {
		c.skip = true
		return c
	}
	// IMDSv2: PUT a token (best-effort).
	req, _ := http.NewRequest(http.MethodPut, imdsBase+"/api/token", nil)
	req.Header.Set("X-aws-ec2-metadata-token-ttl-seconds", "300")
	if resp, err := c.hc.Do(req); err == nil {
		defer resp.Body.Close()
		if b, err := io.ReadAll(resp.Body); err == nil {
			c.token = strings.TrimSpace(string(b))
		}
	}
	return c
}

func (c *imdsClient) get(path string) string {
	if c.skip {
		return ""
	}
	req, err := http.NewRequest(http.MethodGet, imdsBase+path, nil)
	if err != nil {
		return ""
	}
	if c.token != "" {
		req.Header.Set("X-aws-ec2-metadata-token", c.token)
	}
	resp, err := c.hc.Do(req)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return ""
	}
	b, _ := io.ReadAll(resp.Body)
	return strings.TrimSpace(string(b))
}

// gatherNodeInfo builds NodeInfo from IMDS, with env overrides for fields IMDS
// can't always provide (Role/Stack require "instance metadata tags" enabled).
func gatherNodeInfo() proto.NodeInfo {
	c := newIMDS()
	az := c.get("/meta-data/placement/availability-zone")
	region := c.get("/meta-data/placement/region")
	if region == "" && len(az) > 1 {
		region = az[:len(az)-1] // strip AZ suffix letter
	}
	host, _ := os.Hostname()
	n := proto.NodeInfo{
		InstanceID:     firstNonEmpty(c.get("/meta-data/instance-id"), env("AGENT_INSTANCE_ID"), host),
		PrivateIP:      firstNonEmpty(c.get("/meta-data/local-ipv4"), env("AGENT_PRIVATE_IP")),
		PublicIP:       c.get("/meta-data/public-ipv4"),
		AZ:             az,
		Region:         firstNonEmpty(region, env("AWS_REGION")),
		InstanceType:   c.get("/meta-data/instance-type"),
		PlacementGroup: firstNonEmpty(c.get("/meta-data/placement/group-name"), env("AGENT_PG")),
		Role:           firstNonEmpty(c.get("/meta-data/tags/instance/Role"), env("AGENT_ROLE")),
		Stack:          c.get("/meta-data/tags/instance/aws:cloudformation:stack-name"),
		Hostname:       host,
		// No native IMDS path for tenancy - AGENT_TENANCY is written into
		// /etc/default/afxdp-agent by the fleet stack's UserData, which knows
		// the value it requested at launch.
		Tenancy: firstNonEmpty(env("AGENT_TENANCY"), "shared"),
	}
	// VPC and subnet: both hang off the primary ENI's MAC metadata path.
	if mac := c.get("/meta-data/mac"); mac != "" {
		n.VpcID = c.get("/meta-data/network/interfaces/macs/" + mac + "/vpc-id")
		n.SubnetID = c.get("/meta-data/network/interfaces/macs/" + mac + "/subnet-id")
	}
	return n
}

func env(k string) string { return strings.TrimSpace(os.Getenv(k)) }

func firstNonEmpty(vs ...string) string {
	for _, v := range vs {
		if strings.TrimSpace(v) != "" {
			return strings.TrimSpace(v)
		}
	}
	return ""
}
