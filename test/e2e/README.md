# E2E Integration Tests

End-to-end integration tests for the trading latency benchmark suite.

## Quick Start

```bash
export SSH_KEY_FILE=~/.ssh/virginia.pem

# Full run: build AMI → deploy single instance → run test → validate → cleanup
./e2e_integration_test.sh

# Use existing tuned AMI (skips AMI build and OS tuning)
./e2e_integration_test.sh --base-ami ami-xxxxxxxxx

# Keep resources after success for inspection
./e2e_integration_test.sh --no-cleanup

# Or pass the key explicitly
./e2e_integration_test.sh --key-file $SSH_KEY_FILE

# Resume from step 5 (start server) after fixing an issue
./e2e_integration_test.sh --start-from-step 5 --base-ami ami-xxxxxxxxx --no-cleanup
```

## What It Does

1. Builds an OS-tuned AMI via `deployment/build-tuned-ami.sh`
2. Deploys a single EC2 instance (client + server on localhost) using CDK
3. Provisions the instance with Java client and Rust server via Ansible
4. Applies OS tuning (CPU isolation, network, memory)
5. Starts the mock trading server and latency test client
6. Waits for the configured test duration
7. Stops the test and fetches histogram logs
8. Validates that results were produced and are non-empty
9. On success: tears down all AWS resources (unless `--no-cleanup`)
10. On failure: keeps resources and prints manual cleanup commands

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `-k, --key-file` | `$SSH_KEY_FILE` | Path to SSH private key (falls back to env var) |
| `-r, --region` | `us-east-1` | AWS region |
| `--client-instance-type` | `c6in.2xlarge` | EC2 instance type |
| `--test-duration` | `120` | Latency test duration in seconds |
| `--test-size` | `10` | Number of trading rounds per client |
| `--base-ami` | | Use existing tuned AMI (skips build + OS tuning) |
| `--no-cleanup` | `false` | Keep resources after successful test |
| `--start-from-step` | `1` | Resume from step N (1-9), skipping earlier steps |

## Architecture

The e2e test uses `TradingBenchmarkSingleInstanceStack` with `singleEc2Instance=true`,
deploying a single EC2 instance that runs both the mock trading server and the Java HFT
client on localhost. This simplifies the test infrastructure and avoids capacity issues
with provisioning multiple large instances.

## Logs

All logs are written to `test/e2e/logs/<timestamp>/`:
- `e2e_test.log` — main test output
- `ami_build.log` — AMI builder output
- `cdk_deploy.log` — CDK deployment output
- `ansible_*.log` — per-playbook Ansible output
- `cluster-outputs.json` — CDK stack outputs
- `*.hlog` — fetched histogram results
- `*_report.txt` — latency reports (if JAR available locally)
