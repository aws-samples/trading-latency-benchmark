# dev/

Developer iteration tooling — separate from production `deploy/`. Used to build,
test, and hot-deploy code to a running fleet during development.

```
dev/
├── tests/            pytest integration suite (kernel-mode; see tests/README.md)
├── docker/           Dockerfile — local build + test harness (mirrors the AMI bake)
└── ansible/          dev playbooks + shared-inventory symlink
    ├── sync.yaml         rsync local source → EC2s, rebuild, restart replicator
    ├── provision.yaml    full install on stock AL2023 (no baked AMI)
    ├── run_tests.yaml    run the pytest suite on the fleet
    └── inventory.aws_ec2.yml → symlink to ../../deploy/ansible/inventory.aws_ec2.yml
```

## docker/ — local build + test

Mirrors the AMI bake (xdp-tools + `make full`), then runs pytest in kernel mode
(no root/XDP). Validates that the code compiles exactly as the bake will, before
spending ~12 min on an EC2 bake. Build for `linux/amd64` (the Makefile targets
x86_64; emulated on Apple Silicon):

```bash
# from af_xdp/
docker build --platform linux/amd64 -f dev/docker/Dockerfile -t afxdp-test .
docker run  --rm --platform linux/amd64 afxdp-test            # runs pytest dev/tests/
```

## ansible/ — dev iteration on a running fleet

Run from `dev/ansible/` (the inventory is symlinked here). Requires
`SSH_KEY_FILE` and `AWS_DEFAULT_REGION`, and a fleet tagged by `Role`.

```bash
cd dev/ansible
export SSH_KEY_FILE=~/.ssh/your-key.pem AWS_DEFAULT_REGION=us-east-1

# Hot-deploy local code changes to the running fleet (rsync + make full + restart):
ansible-playbook -i inventory.aws_ec2.yml sync.yaml

# Full provision of stock AL2023 instances (no baked AMI):
ansible-playbook -i inventory.aws_ec2.yml provision.yaml
ansible-playbook -i inventory.aws_ec2.yml provision.yaml -e rebuild=true   # rebuild only

# Run the integration test suite on the fleet (stops replicator during pytest):
ansible-playbook -i inventory.aws_ec2.yml run_tests.yaml
```

`sync.yaml` excludes build artifacts (`*.o`, `*.d`, binaries) from the rsync so a
stale host object never contaminates the remote build; both `sync.yaml` and
`provision.yaml` also apply the network/rmem tuning and CPU-isolation config.

See [`tests/README.md`](tests/README.md) for the test suite details.
