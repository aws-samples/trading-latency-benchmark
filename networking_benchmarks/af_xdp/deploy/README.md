# Infrastructure and provisioning for the AF_XDP latency benchmark.

## Structure

```
deploy/
├── cdk/                   CDK infrastructure (Fleet + AMI Builder stacks)
│   ├── bin/app.ts         Entry point — deployment type routing
│   ├── lib/               Stack constructs (FleetStack, AmiBuilderStack)
│   ├── scenarios/         Pre-built fleet topologies (ucast/ + mcast/)
│   └── scripts/           AMI bake script (configs, binaries, systemd)
│
└── ansible/               Runtime/benchmark playbooks + shared inventory
    ├── run_ucast.yaml     Unicast NxN RTT benchmark + report
    ├── run_mcast.yaml     Multicast fan-out benchmark
    ├── configure_mcast.yaml  Multicast runtime setup (GRE, replicator, registration)
    └── inventory.aws_ec2.yml  Dynamic EC2 inventory by Role tag

Dev/iteration tooling lives outside deploy/, under af_xdp/dev/:

    dev/
    ├── tests/             pytest integration suite (kernel-mode)
    ├── docker/Dockerfile  local build + test harness (mirrors the AMI bake)
    └── ansible/           provision.yaml, sync.yaml, run_tests.yaml (+ inventory symlink)
```

## Deployment Flow

### Production (baked AMI)

```
1. Build AMI (~9 min, one-time)
   cdk deploy --context deploymentType=ami-builder --context keyPairName=virginia

2. Deploy fleet (instant readiness)
   cdk deploy --context scenario=ucast/az-cpg-2 --context amiId=ami-xxx

3. (Multicast only) Configure topology
   ansible-playbook configure_mcast.yaml -e replicator_private_ip=10.61.0.5

4. Run benchmarks
   ssh ec2-user@<node> '/opt/af-xdp/rtt <target_ip> 5000 ...'
```

### Development (stock AL2023)

```
1. Deploy fleet (stock AMI)
   cdk deploy --context scenario=ucast/az-cpg-2

2. Provision instances (~8 min)
   ansible-playbook provision.yaml

3. (Multicast only) Configure topology
   ansible-playbook configure_mcast.yaml -e replicator_private_ip=10.61.0.5

4. Iterate
   ansible-playbook provision.yaml -e rebuild=true    # after code changes
```

### Self-hosted (BYOI — no CDK)

Use ansible directly on your own instances. Tag them with `Role: source/replicator/destination`, ensure SSH + intra-fleet connectivity, then run:

```
1. Tag instances (AWS console, CLI, or Terraform)
   aws ec2 create-tags --resources i-xxx --tags Key=Role,Value=replicator

2. Provision
   ansible-playbook -i inventory.aws_ec2.yml provision.yaml

3. Run benchmarks (unicast — no further config needed)
   ssh ec2-user@<node> '/opt/af-xdp/rtt <target> 5000 ...'
```

See [ansible/README.md](ansible/README.md) for static inventory examples and full BYOI documentation.

## Instance Roles

Assigned via CDK fleet spec `role` field (or manual EC2 tags for BYOI), used as ansible groups:

| Role | Description | Baked behavior (on boot) |
|------|-------------|--------------------------|
| `source` | Market data origin (exchange simulator) | Replicator in kernel-mode |
| `replicator` | Packet replicator (AF_XDP or kernel) | Replicator in kernel-mode |
| `destination` | Latency measurement endpoint | Replicator in kernel-mode |

All nodes boot the same — role determines topology wiring at runtime (via ansible or test scripts).

## Key Decisions

- **Universal AMI:** One AMI for all roles — no per-role variants needed
- **Replicator on boot:** Every instance starts `replicator.service` in unicast mode by default (ready for RTT immediately)
- **Mode switching:** `/etc/default/replicator` controls mode (kernel/unicast/mcast), port, and mcast group
- **No ansible for unicast:** Baked AMI instances are test-ready on boot
- **Ansible for multicast only:** GRE tunnels + destination registration require runtime IPs

## Documentation

- [CDK README](cdk/README.md) — stacks, parameters, fleet spec, scenarios
- [Ansible README](ansible/README.md) — playbooks, inventory, variables
- [CDK lib/ README](cdk/lib/README.md) — FleetStack + AmiBuilderStack internals
- [Scenarios README](cdk/scenarios/README.md) — topology table, custom format
- [Scripts README](cdk/scripts/README.md) — bake-ami.sh flow, configs applied
