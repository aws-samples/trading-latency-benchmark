import * as cdk from 'aws-cdk-lib';
import * as ec2 from 'aws-cdk-lib/aws-ec2';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as route53 from 'aws-cdk-lib/aws-route53';
import { Tags, RemovalPolicy } from 'aws-cdk-lib';

export interface ControlPlaneStackProps extends cdk.StackProps {
  keyPairName: string;
  instanceType?: string;
  gitRepo?: string;
  gitRef?: string;
  /** CIDR allowed to reach NATS (4222) + web (8080). Default: anywhere (agents live
   *  in other VPCs/regions). Restrict in production and add NATS auth/TLS. */
  clientCidr?: string;
  /** Optional Route53: both required to create a DNS A record for the web/NATS host. */
  hostedZoneId?: string;
  zoneName?: string;
  recordName?: string; // e.g. bench.example.com
  /** NATS auth token. If omitted, one is generated on the host at boot. */
  natsToken?: string;
  /** Enable TLS on NATS (self-signed cert generated on the host; agents skip-verify). */
  natsTls?: boolean;
}

/**
 * ControlPlaneStack — a single dedicated EC2 that runs nats-server + the Go
 * backend (which serves the web app + SSE/JSON API). Fleet agents connect
 * OUTBOUND to this host's NATS endpoint (public EIP), so no VPC peering is
 * needed across the per-scenario fleet VPCs. The NATS URL is published to SSM
 * (/af-xdp/nats-url) so baked agents self-discover it at boot.
 *
 * NOTE: the userData clones the repo and builds the backend, so the
 * control-plane Go code must be committed + pushed to gitRepo@gitRef.
 */
export class ControlPlaneStack extends cdk.Stack {
  constructor(scope: cdk.App, id: string, props: ControlPlaneStackProps) {
    super(scope, id, props);

    const instanceType = props.instanceType ?? 't3.small';
    const gitRepo = props.gitRepo ?? 'https://github.com/aws-samples/trading-latency-benchmark.git';
    const gitRef = props.gitRef ?? 'main';
    const clientCidr = props.clientCidr ?? '0.0.0.0/0';

    // ── VPC (public subnet; the host needs a public EIP for cross-VPC agents) ─
    const vpc = new ec2.Vpc(this, 'Vpc', {
      natGateways: 0,
      maxAzs: 1,
      subnetConfiguration: [{ cidrMask: 24, name: 'Public', subnetType: ec2.SubnetType.PUBLIC, mapPublicIpOnLaunch: true }],
    });

    // ── Security group ────────────────────────────────────────────────────────
    const sg = new ec2.SecurityGroup(this, 'Sg', {
      vpc, description: 'control plane: ssh + nats + web', allowAllOutbound: true,
    });
    sg.addIngressRule(ec2.Peer.anyIpv4(), ec2.Port.tcp(22), 'SSH');
    sg.addIngressRule(ec2.Peer.ipv4(clientCidr), ec2.Port.tcp(4222), 'NATS (agents)');
    sg.addIngressRule(ec2.Peer.ipv4(clientCidr), ec2.Port.tcp(8080), 'web + API');

    // ── IAM: SSM core + publish the NATS endpoint parameter ───────────────────
    const role = new iam.Role(this, 'Role', {
      assumedBy: new iam.ServicePrincipal('ec2.amazonaws.com'),
      managedPolicies: [iam.ManagedPolicy.fromAwsManagedPolicyName('AmazonSSMManagedInstanceCore')],
    });
    role.addToPolicy(new iam.PolicyStatement({
      actions: ['ssm:PutParameter'],
      resources: [cdk.Arn.format({ service: 'ssm', resource: 'parameter', resourceName: 'af-xdp/*' }, this)],
    }));

    // ── Elastic IP (stable endpoint for agents + DNS) ─────────────────────────
    const eip = new ec2.CfnEIP(this, 'Eip', { domain: 'vpc' });

    // ── UserData: build + run nats-server & backend, publish SSM endpoint ─────
    const ud = ec2.UserData.forLinux();
    ud.addCommands(
      'set -uo pipefail',
      'exec > /var/log/cp-setup.log 2>&1',
      'echo "=== control-plane setup $(date -u) ==="',
      'dnf install -y git nodejs npm || { curl -fsSL https://rpm.nodesource.com/setup_20.x | bash - && dnf install -y git nodejs; } || dnf install -y git',
      'GOVER=$(curl -sL "https://go.dev/VERSION?m=text" | head -1)',
      'curl -fsSL "https://go.dev/dl/${GOVER}.linux-amd64.tar.gz" -o /tmp/go.tgz && rm -rf /usr/local/go && tar -C /usr/local -xzf /tmp/go.tgz',
      'export PATH=/usr/local/go/bin:$PATH GOFLAGS="-mod=mod -buildvcs=false" GOCACHE=/tmp/gocache GOPATH=/tmp/go',
      'mkdir -p /opt/af-xdp-cp',
      `git clone --depth 1 --branch ${gitRef} ${gitRepo} /opt/cp-src`,
      'CP=/opt/cp-src/networking_benchmarks/af_xdp/control-plane',
      '( cd "$CP" && go build -o /opt/af-xdp-cp/afxdp-backend ./backend ) && echo "backend built"',
      '( cd "$CP/web" && npm install && npm run build && cp -r dist /opt/af-xdp-cp/web-dist ) || echo "web build failed (check /var/log/cp-setup.log)"',
      'NATSVER=v2.10.22',
      'curl -fsSL "https://github.com/nats-io/nats-server/releases/download/${NATSVER}/nats-server-${NATSVER}-linux-amd64.tar.gz" -o /tmp/nats.tgz && tar xzf /tmp/nats.tgz -C /tmp && cp /tmp/nats-server-*/nats-server /opt/af-xdp-cp/nats-server && chmod +x /opt/af-xdp-cp/nats-server',
      // Auth token (provided or generated) + TLS flag.
      `NATS_TOKEN='${(props.natsToken || '').replace(/'/g, '')}'`,
      '[ -z "$NATS_TOKEN" ] && NATS_TOKEN=$(openssl rand -hex 24)',
      `NATS_TLS=${props.natsTls ? '1' : '0'}`,
      // nats.conf: token authorization (+ optional self-signed TLS).
      'cat > /opt/af-xdp-cp/nats.conf <<CONF\nport: 4222\nauthorization { token: "$NATS_TOKEN" }\nCONF',
      'SCHEME=nats; BE_TLS=""',
      'if [ "$NATS_TLS" = "1" ]; then\n  openssl req -x509 -newkey rsa:2048 -nodes -keyout /opt/af-xdp-cp/key.pem -out /opt/af-xdp-cp/cert.pem -days 3650 -subj "/CN=afxdp-cp" >/dev/null 2>&1\n  cat >> /opt/af-xdp-cp/nats.conf <<TLSC\ntls {\n  cert_file: "/opt/af-xdp-cp/cert.pem"\n  key_file: "/opt/af-xdp-cp/key.pem"\n}\nTLSC\n  SCHEME=tls; BE_TLS="-nats-insecure"\nfi',
      'cat > /etc/systemd/system/cp-nats.service <<UNIT\n[Unit]\nDescription=NATS server (control plane)\nAfter=network-online.target\nWants=network-online.target\n[Service]\nExecStart=/opt/af-xdp-cp/nats-server -c /opt/af-xdp-cp/nats.conf -a 0.0.0.0\nRestart=always\nRestartSec=3\n[Install]\nWantedBy=multi-user.target\nUNIT',
      'cat > /etc/systemd/system/cp-backend.service <<UNIT\n[Unit]\nDescription=AF_XDP control-plane backend\nAfter=cp-nats.service\nWants=cp-nats.service\n[Service]\nExecStart=/opt/af-xdp-cp/afxdp-backend -nats ${SCHEME}://127.0.0.1:4222 -nats-token ${NATS_TOKEN} ${BE_TLS} -addr :8080 -web /opt/af-xdp-cp/web-dist\nRestart=always\nRestartSec=3\n[Install]\nWantedBy=multi-user.target\nUNIT',
      'systemctl daemon-reload && systemctl enable --now cp-nats.service cp-backend.service',
      `aws ssm put-parameter --region ${this.region} --name /af-xdp/nats-url --type String --overwrite --value "$SCHEME://${eip.ref}:4222" || true`,
      `aws ssm put-parameter --region ${this.region} --name /af-xdp/nats-token --type String --overwrite --value "$NATS_TOKEN" || true`,
      `echo "=== control-plane up: $SCHEME://${eip.ref}:4222  web http://${eip.ref}:8080 ==="`,
    );

    // ── Instance ──────────────────────────────────────────────────────────────
    const instance = new ec2.Instance(this, 'Host', {
      vpc,
      instanceType: new ec2.InstanceType(instanceType),
      machineImage: ec2.MachineImage.latestAmazonLinux2023(),
      securityGroup: sg,
      role,
      keyPair: ec2.KeyPair.fromKeyPairName(this, 'KeyPair', props.keyPairName),
      blockDevices: [{ deviceName: '/dev/xvda', volume: ec2.BlockDeviceVolume.ebs(20) }],
      userData: ud,
    });
    instance.applyRemovalPolicy(RemovalPolicy.DESTROY);
    Tags.of(instance).add('Name', 'af-xdp-control-plane');

    new ec2.CfnEIPAssociation(this, 'EipAssoc', { allocationId: eip.attrAllocationId, instanceId: instance.instanceId });

    // ── Optional Route53 A record ─────────────────────────────────────────────
    if (props.hostedZoneId && props.zoneName && props.recordName) {
      const zone = route53.HostedZone.fromHostedZoneAttributes(this, 'Zone', {
        hostedZoneId: props.hostedZoneId, zoneName: props.zoneName,
      });
      new route53.ARecord(this, 'Record', {
        zone, recordName: props.recordName,
        target: route53.RecordTarget.fromIpAddresses(eip.ref),
        ttl: cdk.Duration.minutes(5),
      });
      new cdk.CfnOutput(this, 'WebUrlDns', { value: `http://${props.recordName}:8080` });
      new cdk.CfnOutput(this, 'NatsUrlDns', { value: `nats://${props.recordName}:4222` });
    }

    // ── Outputs ───────────────────────────────────────────────────────────────
    new cdk.CfnOutput(this, 'PublicIp', { value: eip.ref });
    new cdk.CfnOutput(this, 'WebUrl', { value: `http://${eip.ref}:8080` });
    new cdk.CfnOutput(this, 'NatsUrl', { value: `nats://${eip.ref}:4222` });
    new cdk.CfnOutput(this, 'SsmNatsParam', { value: '/af-xdp/nats-url' });
  }
}
