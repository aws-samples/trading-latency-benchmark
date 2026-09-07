import * as cdk from 'aws-cdk-lib';
import * as ec2 from 'aws-cdk-lib/aws-ec2';
import * as ssm from 'aws-cdk-lib/aws-ssm';
import * as cr from 'aws-cdk-lib/custom-resources';
import * as iam from 'aws-cdk-lib/aws-iam';
import {
  Vpc,
  SubnetType,
  Instance,
  InstanceType,
  MachineImage,
  SecurityGroup,
  Port,
  Peer,
  BlockDeviceVolume,
  CfnPlacementGroup,
  CfnHost,
  UserData,
  KeyPair,
} from 'aws-cdk-lib/aws-ec2';
import { Tags, RemovalPolicy } from 'aws-cdk-lib';

// DEFAULT is m8a.2xlarge = 8 vCPU / 8 physical cores (AMD, no SMT -> all 8
// online). MULTICAST: each node runs a
// single busy-poll app so it needs only 3 dedicated cores (OS, ENA IRQ, app).
// UNICAST is heavier: the sender co-locates the replicator poll thread AND the
// rtt sender AND the rtt receiver, and each wants its own physical core
// alongside OS + a SEPARATE ENA-IRQ core (keeping the IRQ off the poll core
// avoids the tail jitter that hit --xdp-tx) = 5 cores. One AMI serves both:
// core pinning is derived dynamically at runtime from the isolated set
// (bake-ami.sh isolcpus, the replicator's initializeCpuCores, and
// run_ucast.yaml auto_pin), so it adapts to whatever instance a scenario
// deploys — 2xlarge -> metal, Intel -> AMD (see Makefile's -march=x86-64-v3,
// which keeps the built binary portable across both vendor families).
const DEFAULT_INSTANCE_TYPE = 'm8a.2xlarge';
const DEFAULT_ROLE = 'destination';
const DEFAULT_PRIMARY_CIDR = '10.61.0.0/16';
const DEFAULT_SECONDARY_CIDR = '10.62.0.0/16';
const CONTROL_PORT = 12345;

export type PlacementStrategy = 'cluster' | 'spread' | 'partition';

/** EC2 tenancy: "shared" (default, multi-tenant host), "instance" (Dedicated
 *  Instance - single-tenant hardware, no placement control), or "host"
 *  (Dedicated Host - pinned physical server, allocated per (instanceType,AZ)
 *  group and shared by every entry in that group). */
export type Tenancy = 'shared' | 'instance' | 'host';

/** A single node in the fleet specification. */
export interface FleetEntry {
  /** EC2 instance type. Default: m8a.2xlarge */
  type?: string;
  /** Number of instances. Default: 1 */
  count?: number;
  /** Logical role: "source", "replicator", "destination". Default: "destination" */
  role?: string;
  /** AZ suffix (e.g. "a") or full AZ name (e.g. "us-east-1a"). Default: first AZ of region. */
  az?: string;
  /** Placement strategy: "cluster", "spread", "partition". Default: none. */
  pgType?: PlacementStrategy;
  /** Placement group / reporting label. Entries with the same name share a PG. */
  pgName?: string;
  /** AWS region (e.g. "us-east-1", "eu-west-2"). Default: stack's region.
   *  Entries whose region differs from the primary become a second stack. */
  region?: string;
  /** EC2 tenancy: "shared" (default), "instance" (Dedicated Instance -
   *  single-tenant hardware isolation, no placement control/guarantee - see
   *  dev/roadmap for the latency-impact writeup), or "host" (Dedicated Host -
   *  pinned physical server; every entry sharing the same (type, AZ) is
   *  allocated onto the same host, capacity permitting). */
  tenancy?: Tenancy;
  /** Target a Dedicated Host instead of letting this stack allocate a fresh
   *  one per (type, AZ). Two forms:
   *   - A real AWS host ID ("h-0123456789abcdef0") - targets that exact,
   *     already-allocated host. No CfnHost is created.
   *   - Any other string ("a", "my-host") - a logical ALIAS. Every entry
   *     using the same alias shares ONE newly-allocated CfnHost, even though
   *     no real host ID was ever provided. This is the common case: name a
   *     host once per scenario and reuse the name across entries/rows,
   *     rather than wiring real IDs together or relying on (type, AZ)
   *     grouping (which breaks if two logically-separate host groups happen
   *     to share the same type and AZ).
   *  Requires tenancy:"host".
   */
  hostId?: string;
}

/** A real AWS Dedicated Host ID looks like "h-" followed by 17 hex chars
 *  (matches the instance-id/volume-id/etc. shape EC2 uses account-wide).
 *  Anything else passed as hostId is a logical alias, not a literal ID. */
export function isRealHostId(v: string): boolean {
  return /^h-[0-9a-f]{17}$/.test(v);
}

/** A fleet entry with its AZ resolved to a full AZ name. */
export type ResolvedEntry = FleetEntry & { _resolvedAz: string };

export interface FleetStackProps extends cdk.StackProps {
  /** SSH key pair name (must exist in this stack's region). */
  keyPairName: string;
  /** Custom AMI ID for this region. Default: SSM-resolved (primary) or AL2023. */
  amiId?: string;
  /** VPC CIDR for this region. */
  vpcCidr?: string;
  /** UDP data port (SG rules). Default: 5000 */
  dataPort?: number;
  /** Resolved fleet entries that belong to THIS stack's region. */
  entries: ResolvedEntry[];
  /** This stack's region (explicit - env.region may be a token at synth). */
  regionName: string;
  /** Peer region VPC CIDR - opens SG ingress for cross-region data/control. */
  peerVpcCidr?: string;
  /** Resolve the AMI from SSM /af-xdp/ami/<region> (primary region only). */
  ssmAmi?: boolean;
  /** Region where the control-plane publishes /af-xdp/nats-url. Set on
   *  secondary stacks so user-data replicates the params locally. */
  controlPlaneRegion?: string;
  /** CIDR allowed to SSH into fleet nodes. Omitted: SSH is open to
   *  0.0.0.0/0 - only rely on that fallback for throwaway/local testing.
   *  `afxdpctl up` always supplies this (auto-detected caller IP by
   *  default), so it is unset only when the CDK app is invoked directly. */
  adminCidr?: string;
}

/** Resolve an AZ spec ("a" | "us-east-1a" | undefined) to a full AZ name. */
export function resolveAz(azSpec: string | undefined, region: string): string {
  if (!azSpec) return `${region}a`;
  if (azSpec.includes('-')) return azSpec;
  return `${region}${azSpec}`;
}

/** Partition a fleet into primary/secondary regions and resolve AZs. */
export function partitionFleet(fleet: FleetEntry[], primaryRegion: string): {
  primaryEntries: ResolvedEntry[];
  secondaryEntries: ResolvedEntry[];
  secondaryRegion?: string;
} {
  const primaryEntries: ResolvedEntry[] = [];
  const secondaryEntries: ResolvedEntry[] = [];
  let secondaryRegion: string | undefined;

  for (const entry of fleet) {
    const entryRegion = entry.region ?? primaryRegion;
    const isSecondary = entryRegion !== primaryRegion;
    if (isSecondary) {
      if (secondaryRegion && secondaryRegion !== entryRegion) {
        throw new Error(
          `Only one secondary region is supported. Got entries for both "${secondaryRegion}" and "${entryRegion}".`
        );
      }
      secondaryRegion = entryRegion;
    }
    const resolved: ResolvedEntry = { ...entry, _resolvedAz: resolveAz(entry.az, entryRegion) };
    (isSecondary ? secondaryEntries : primaryEntries).push(resolved);
  }

  if (primaryEntries.length === 0) {
    throw new Error('Fleet must have at least one entry in the primary region');
  }
  return { primaryEntries, secondaryEntries, secondaryRegion };
}

const VALID_TENANCIES: Tenancy[] = ['shared', 'instance', 'host'];

/** Validate placement-group constraints for one region's entries. */
function validateEntries(entries: ResolvedEntry[]): void {
  const clusterGroupAZs = new Map<string, Set<string>>();
  const spreadPerAz = new Map<string, number>();
  const hostAliasSpecs = new Map<string, { type: string; az: string }>();

  for (const entry of entries) {
    const count = entry.count ?? 1;
    const az = entry._resolvedAz;
    const tenancy = entry.tenancy ?? 'shared';
    if (!VALID_TENANCIES.includes(tenancy)) {
      throw new Error(`Invalid tenancy "${tenancy}". Must be one of: ${VALID_TENANCIES.join(', ')}`);
    }
    if (entry.hostId && tenancy !== 'host') {
      throw new Error(`hostId "${entry.hostId}" was set but tenancy is "${tenancy}" - hostId requires tenancy:"host".`);
    }
    if (entry.hostId && !isRealHostId(entry.hostId)) {
      // Alias form: every entry sharing this alias becomes ONE CfnHost, so
      // they must agree on the instance type and AZ that host is allocated
      // with - unlike pgName (a placement group can span any instance
      // types), a host is fixed to one type once allocated.
      const instType = entry.type ?? DEFAULT_INSTANCE_TYPE;
      const spec = hostAliasSpecs.get(entry.hostId);
      if (!spec) {
        hostAliasSpecs.set(entry.hostId, { type: instType, az });
      } else if (spec.type !== instType || spec.az !== az) {
        throw new Error(
          `hostId alias "${entry.hostId}" is used with inconsistent (type, AZ): ` +
          `got (${spec.type}, ${spec.az}) and (${instType}, ${az}). ` +
          `All entries sharing a host alias must use the same instance type and AZ.`
        );
      }
    }
    if (entry.pgType === 'cluster') {
      const group = entry.pgName ?? '__default__';
      if (!clusterGroupAZs.has(group)) clusterGroupAZs.set(group, new Set());
      clusterGroupAZs.get(group)!.add(az);
    }
    if (entry.pgType === 'spread') {
      // AWS requires "default" tenancy for spread placement groups; cluster
      // and partition accept "default" or "dedicated" (see
      // docs.aws.amazon.com/cli/latest/reference/ec2/modify-instance-placement.html).
      // The same restriction applies to Dedicated Host ("host") tenancy -
      // there is no "spread" + host-affinity combination in the API.
      if (tenancy !== 'shared') {
        throw new Error(`tenancy "${tenancy}" is not supported with a "spread" placement group.`);
      }
      spreadPerAz.set(az, (spreadPerAz.get(az) ?? 0) + count);
    }
  }

  for (const [group, azs] of clusterGroupAZs) {
    if (azs.size > 1) {
      const label = group === '__default__' ? '(unnamed)' : `"${group}"`;
      throw new Error(
        `Cluster placement group ${label} requires all instances in the same AZ. ` +
        `Got: ${Array.from(azs).join(', ')}`
      );
    }
  }
  for (const [az, count] of spreadPerAz) {
    if (count > 7) {
      throw new Error(`Spread placement max 7 per AZ. Got ${count} in ${az}.`);
    }
  }
}

/**
 * Region-scoped deployment stack.
 *
 * Builds a VPC, security group, placement groups and instances for the fleet
 * entries that belong to ITS region. A CloudFormation stack is single-region,
 * so cross-region topologies use TWO FleetStacks (one per region) wired
 * together by {@link connectRegions}.
 */
export class FleetStack extends cdk.Stack {
  public readonly vpc: Vpc;
  public readonly sg: SecurityGroup;
  public readonly vpcCidr: string;
  public readonly dataPort: number;

  constructor(scope: cdk.App, id: string, props: FleetStackProps) {
    super(scope, id, props);

    const region = props.regionName;
    const dataPort = props.dataPort ?? 5000;
    const vpcCidr = props.vpcCidr ?? DEFAULT_PRIMARY_CIDR;
    this.dataPort = dataPort;
    this.vpcCidr = vpcCidr;

    validateEntries(props.entries);

    const azs = Array.from(new Set(props.entries.map(e => e._resolvedAz))).sort();
    if (azs.length === 0) {
      throw new Error(`No entries for region ${region}`);
    }

    // ── VPC ──────────────────────────────────────────────────────────────
    const vpc = new Vpc(this, 'Vpc', {
      ipAddresses: ec2.IpAddresses.cidr(vpcCidr),
      natGateways: 0,
      availabilityZones: azs,
      subnetConfiguration: [{
        cidrMask: 24,
        name: 'Public',
        subnetType: SubnetType.PUBLIC,
        mapPublicIpOnLaunch: true,
      }],
      gatewayEndpoints: {},
    });
    vpc.applyRemovalPolicy(RemovalPolicy.DESTROY);
    this.vpc = vpc;

    // ── Security group ─────────────────────────────────────────────────────
    const sg = new SecurityGroup(this, 'Sg', {
      vpc,
      description: 'AF_XDP benchmark: SSH + intra-group + cross-region data',
      allowAllOutbound: true,
    });
    sg.applyRemovalPolicy(RemovalPolicy.DESTROY);
    if (props.adminCidr) {
      sg.addIngressRule(Peer.ipv4(props.adminCidr), Port.tcp(22), 'SSH (admin)');
    } else {
      sg.addIngressRule(Peer.anyIpv4(), Port.tcp(22), 'SSH');
    }
    sg.addIngressRule(sg, Port.allTraffic(), 'All intra-group traffic');
    if (props.peerVpcCidr) {
      // Mirror the intra-group allowance for the peer VPC. An SG self-reference
      // does not span cross-region peering, so enumerating only the data and
      // control ports left the rtt echo port (19020) closed: requests arrived,
      // echoes were dropped, and every cross-region pair reported 100% loss.
      sg.addIngressRule(Peer.ipv4(props.peerVpcCidr), Port.allTraffic(), 'All traffic from peer region');
    }
    this.sg = sg;

    // ── Placement groups ───────────────────────────────────────────────────
    const placementGroups = new Map<string, CfnPlacementGroup>();
    const getOrCreatePG = (strategy: PlacementStrategy, az: string, groupName?: string): CfnPlacementGroup => {
      const key = groupName
        ? `${strategy}:${groupName}`
        : (strategy === 'cluster' ? `cluster:${az}` : strategy);
      if (!placementGroups.has(key)) {
        const sanitized = (groupName ?? az).replace(/[^a-zA-Z0-9]/g, '');
        const pgId = groupName
          ? `PG-${strategy}-${sanitized}`
          : (strategy === 'cluster' ? `PG-cluster-${sanitized}` : `PG-${strategy}`);
        const pg = new CfnPlacementGroup(this, pgId, { strategy });
        pg.applyRemovalPolicy(RemovalPolicy.DESTROY);
        placementGroups.set(key, pg);
      }
      return placementGroups.get(key)!;
    };

    // ── Dedicated Hosts (tenancy: "host") ───────────────────────────────────
    // Two ways a host gets shared across entries:
    //  1. No hostId at all: one CfnHost per (instanceType, AZ) - every entry
    //     requesting "host" tenancy for that combination shares it.
    //  2. hostId is an ALIAS (not a real "h-..." id, see isRealHostId): one
    //     CfnHost per alias, regardless of (type, AZ) grouping - lets a
    //     scenario name a host once ("a") and reuse that name across rows
    //     without ever knowing/providing the real AWS host ID.
    // A real hostId ("h-0123456789abcdef0") skips allocation entirely and
    // targets that exact pre-existing host - no CfnHost is created for it.
    const dedicatedHosts = new Map<string, CfnHost>();
    const getOrCreateHost = (key: string, instType: string, az: string, idPrefix: string): CfnHost => {
      if (!dedicatedHosts.has(key)) {
        const sanitized = `${idPrefix}-${instType}-${az}`.replace(/[^a-zA-Z0-9]/g, '');
        const host = new CfnHost(this, `Host-${sanitized}`, {
          instanceType: instType,
          availabilityZone: az,
          autoPlacement: 'off', // only instances that target this hostId land here
          hostRecovery: 'off',  // hosts are ephemeral (this.applyRemovalPolicy below); no need to auto-recover onto a released host
        });
        host.applyRemovalPolicy(RemovalPolicy.DESTROY);
        dedicatedHosts.set(key, host);
      }
      return dedicatedHosts.get(key)!;
    };

    // ── AMI + key pair ─────────────────────────────────────────────────────
    // Explicit --context amiId/secondaryAmiId always wins. Otherwise, resolve
    // the baked AMI from this region's SSM /af-xdp/ami/<region> (the
    // ami-builder publishes it in each region it bakes). If ssmAmi is false
    // AND no explicit amiId, synth fails - never silently fall back to a
    // stock image that lacks the agent.
    let ami: ec2.IMachineImage;
    if (props.amiId) {
      ami = MachineImage.genericLinux({ [region]: props.amiId });
    } else if (props.ssmAmi) {
      const ssmParam = ssm.StringParameter.valueForStringParameter(this, `/af-xdp/ami/${region}`);
      ami = MachineImage.genericLinux({ [region]: ssmParam });
    } else {
      throw new Error(
        `No AMI for region ${region}. Either bake an AMI there ` +
        `(publishes /af-xdp/ami/${region} to SSM) or pass --context secondaryAmiId=<id>.`
      );
    }
    const keyPair = KeyPair.fromKeyPairName(this, 'KeyPair', props.keyPairName);

    // ── Instances ──────────────────────────────────────────────────────────
    const fleetManifest: {
      index: number; instanceType: string; role: string;
      az: string; region: string; pgType: string | null; pgName: string | null; outputPrefix: string;
      tenancy: Tenancy;
    }[] = [];

    let globalIndex = 0;
    for (const entry of props.entries) {
      const count = entry.count ?? 1;
      const role = entry.role ?? DEFAULT_ROLE;
      const instType = entry.type ?? DEFAULT_INSTANCE_TYPE;
      const az = entry._resolvedAz;
      const pgType = entry.pgType ?? null;
      const tenancy = entry.tenancy ?? 'shared';

      for (let i = 0; i < count; i++) {
        const shortType = instType.replace('.', '-');
        const nodeId = `Node${globalIndex}`;
        const nodeName = `${role}-${globalIndex}-${shortType}`;
        const prefix = `Node${globalIndex}`;

        // Per-node agent config: stamp the role (known here) so the baked
        // control-plane agent knows its role. When this stack is in a
        // secondary region, replicate NATS SSM params from the control-plane's
        // region so the agent's local SSM lookup succeeds.
        const agentUd = UserData.forLinux();
        if (props.controlPlaneRegion && props.controlPlaneRegion !== region) {
          agentUd.addCommands(
          // The agent preflight sources /etc/default/afxdp-agent before falling
          // back to SSM, so writing the endpoint there is enough. Replicating the
          // parameters into this region would need ssm:PutParameter on every node,
          // letting any one of them repoint the whole fleet.
            `# NATS discovery from the control-plane region (${props.controlPlaneRegion})`,
            `NATS_URL=$(aws ssm get-parameter --region ${props.controlPlaneRegion} --name /af-xdp/nats-url --query Parameter.Value --output text)`,
            `NATS_TOKEN=$(aws ssm get-parameter --region ${props.controlPlaneRegion} --name /af-xdp/nats-token --query Parameter.Value --output text)`,
            `echo "AGENT_NATS_URL=$NATS_URL" >> /etc/default/afxdp-agent`,
            `echo "AGENT_NATS_TOKEN=$NATS_TOKEN" >> /etc/default/afxdp-agent`,
          );
        }
        agentUd.addCommands(
          `echo "AGENT_ROLE=${role}" >> /etc/default/afxdp-agent`,
          `echo "AGENT_TENANCY=${tenancy}" >> /etc/default/afxdp-agent`,
          entry.pgName ? `echo "AGENT_PG=${entry.pgName}" >> /etc/default/afxdp-agent` : '# no pgName',
          'systemctl restart afxdp-agent 2>/dev/null || true',
        );

        const inst = new Instance(this, nodeId, {
          vpc,
          instanceType: new InstanceType(instType),
          machineImage: ami,
          securityGroup: sg,
          vpcSubnets: { availabilityZones: [az] },
          keyPair,
          blockDevices: [{ deviceName: '/dev/xvda', volume: BlockDeviceVolume.ebs(100) }],
          userData: agentUd,
        });
        inst.applyRemovalPolicy(RemovalPolicy.DESTROY);
        // Disable source/dest check for replication traffic (fan-out rewrites dst IP/MAC)
        (inst.node.defaultChild as ec2.CfnInstance).sourceDestCheck = false;
        if (tenancy === 'instance') {
          // Dedicated Instance: single-tenant hardware, isolated from other
          // AWS accounts. No placement visibility/control and no host
          // affinity - see dev/roadmap for the full writeup. Compatible with
          // cluster/partition placement groups (validated above), billed
          // per-instance.
          (inst.node.defaultChild as ec2.CfnInstance).tenancy = 'dedicated';
        } else if (tenancy === 'host') {
          // Dedicated Host: pinned to a specific physical server, so two
          // entries requesting the same host land on the SAME one - the
          // only tenancy option with actual placement control. affinity=host
          // + hostId targets that host explicitly, rather than tenancy=host
          // alone (which would still auto-place onto ANY host with matching
          // capacity, undoing the whole point of asking for host tenancy).
          //
          // hostId resolution, in order:
          //  - unset: auto-allocate, grouped by (instanceType, AZ).
          //  - a real "h-..." id: target that pre-existing host directly,
          //    no CfnHost created (reused across deploys, or allocated
          //    out-of-band via `aws ec2 allocate-hosts`).
          //  - anything else: an ALIAS - every entry using the same alias
          //    string shares one newly-allocated CfnHost, keyed on the alias
          //    itself rather than (type, AZ), so a scenario can just name a
          //    host ("a") once and reuse that name without ever knowing a
          //    real host ID (validateEntries already checked every entry
          //    sharing this alias agrees on (type, AZ)).
          let hostIdRef: string;
          if (!entry.hostId) {
            hostIdRef = getOrCreateHost(`${instType}:${az}`, instType, az, 'Host').attrHostId;
          } else if (isRealHostId(entry.hostId)) {
            hostIdRef = entry.hostId;
          } else {
            hostIdRef = getOrCreateHost(`alias:${entry.hostId}`, instType, az, `alias-${entry.hostId}`).attrHostId;
          }
          const cfnInst = inst.node.defaultChild as ec2.CfnInstance;
          cfnInst.tenancy = 'host';
          cfnInst.affinity = 'host';
          cfnInst.hostId = hostIdRef;
        }
        // Read-only SSM: a secondary node receives the endpoint via user-data,
        // so no node ever needs to write a shared parameter.
        inst.role.addToPrincipalPolicy(new iam.PolicyStatement({
          actions: ['ssm:GetParameter'],
          resources: [cdk.Arn.format({ service: 'ssm', resource: 'parameter', resourceName: 'af-xdp/*' }, this)],
        }));
        if (props.controlPlaneRegion && props.controlPlaneRegion !== region) {
          inst.role.addToPrincipalPolicy(new iam.PolicyStatement({
            actions: ['ssm:GetParameter'],
            resources: [
              `arn:aws:ssm:${props.controlPlaneRegion}:${this.account}:parameter/af-xdp/*`,
            ],
          }));
        }
        inst.role.addToPrincipalPolicy(new iam.PolicyStatement({
          actions: ['ec2:DescribeInstanceTypes', 'ec2:DescribePlacementGroups'],
          resources: ['*'],
        }));

        if (pgType) {
          const pg = getOrCreatePG(pgType, az, entry.pgName);
          (inst.node.defaultChild as ec2.CfnInstance).placementGroupName = pg.ref;
          Tags.of(inst).add('PlacementStrategy', pgType);
          if (entry.pgName) Tags.of(inst).add('PlacementGroup', entry.pgName);
        }

        Tags.of(inst).add('Name', nodeName);
        Tags.of(inst).add('Role', role);
        Tags.of(inst).add('InstanceType', instType);
        Tags.of(inst).add('AZ', az);
        Tags.of(inst).add('Region', region);
        Tags.of(inst).add('Tenancy', tenancy);

        new cdk.CfnOutput(this, `${prefix}InstanceId`, { value: inst.instanceId });
        new cdk.CfnOutput(this, `${prefix}PublicIp`, { value: inst.instancePublicIp });
        new cdk.CfnOutput(this, `${prefix}PrivateIp`, { value: inst.instancePrivateIp });

        fleetManifest.push({
          index: globalIndex, instanceType: instType, role, az,
          region, pgType, pgName: entry.pgName ?? null, outputPrefix: prefix, tenancy,
        });
        globalIndex++;
      }
    }

    // ── Outputs ────────────────────────────────────────────────────────────
    new cdk.CfnOutput(this, 'FleetManifest', {
      value: JSON.stringify(fleetManifest),
      description: 'JSON fleet manifest',
    });
    new cdk.CfnOutput(this, 'FleetSize', { value: String(globalIndex) });
    new cdk.CfnOutput(this, 'VpcId', { value: vpc.vpcId });
    new cdk.CfnOutput(this, 'Region', { value: region });
    new cdk.CfnOutput(this, 'AvailabilityZones', { value: azs.join(',') });
    if (placementGroups.size > 0) {
      new cdk.CfnOutput(this, 'PlacementGroups', {
        value: JSON.stringify(Array.from(placementGroups.keys())),
      });
    }
  }
}

/**
 * Wire cross-region VPC peering between a primary and secondary FleetStack.
 *
 * Both stacks must be created with `crossRegionReferences: true`.
 *
 * All peering resources are anchored in the PRIMARY stack to avoid a circular
 * stack dependency (peering needs the secondary VPC id; the secondary route
 * needs the peering id). The primary stack therefore depends on the secondary
 * (for the VPC id + route-table ids); the secondary never depends on the
 * primary. The primary→secondary route is created by an AwsCustomResource that
 * calls ec2:CreateRoute in the secondary region.
 *
 * Same-account cross-region peering is auto-accepted by CloudFormation, so no
 * explicit accept step is needed.
 */
export function connectRegions(
  primary: FleetStack,
  secondary: FleetStack,
  opts: { secondaryRegion: string },
): void {
  const peering = new ec2.CfnVPCPeeringConnection(primary, 'VpcPeering', {
    vpcId: primary.vpc.vpcId,
    peerVpcId: secondary.vpc.vpcId,        // cross-region ref → primary depends on secondary
    peerRegion: opts.secondaryRegion,
    tags: [{ key: 'Name', value: 'PrimaryToSecondaryPeering' }],
  });

  // Primary → secondary routes (native, local to the primary stack).
  primary.vpc.publicSubnets.forEach((subnet, i) => {
    new ec2.CfnRoute(primary, `PriPeerRoute${i}`, {
      routeTableId: subnet.routeTable.routeTableId,
      destinationCidrBlock: secondary.vpcCidr,
      vpcPeeringConnectionId: peering.ref,
    });
  });

  // Secondary → primary routes: created in the secondary region via an
  // AwsCustomResource anchored in the PRIMARY stack (keeps the dependency
  // one-directional: primary → secondary).
  const routePolicy = cr.AwsCustomResourcePolicy.fromStatements([
    new iam.PolicyStatement({
      actions: ['ec2:CreateRoute', 'ec2:DeleteRoute', 'ec2:DescribeRouteTables'],
      resources: ['*'],
    }),
  ]);
  secondary.vpc.publicSubnets.forEach((subnet, i) => {
    const rtbId = subnet.routeTable.routeTableId; // cross-region ref
    new cr.AwsCustomResource(primary, `SecPeerRoute${i}`, {
      resourceType: 'Custom::SecondaryPeerRoute',
      onCreate: {
        service: 'EC2',
        action: 'createRoute',
        region: opts.secondaryRegion,
        parameters: {
          RouteTableId: rtbId,
          DestinationCidrBlock: primary.vpcCidr,
          VpcPeeringConnectionId: peering.ref,
        },
        physicalResourceId: cr.PhysicalResourceId.of(`secroute-${opts.secondaryRegion}-${i}`),
      },
      onDelete: {
        service: 'EC2',
        action: 'deleteRoute',
        region: opts.secondaryRegion,
        parameters: {
          RouteTableId: rtbId,
          DestinationCidrBlock: primary.vpcCidr,
        },
      },
      policy: routePolicy,
      installLatestAwsSdk: false,
    });
  });

  new cdk.CfnOutput(primary, 'PeeringConnectionId', { value: peering.ref });
  new cdk.CfnOutput(primary, 'SecondaryRegion', { value: opts.secondaryRegion });
  new cdk.CfnOutput(primary, 'SecondaryVpcId', { value: secondary.vpc.vpcId });
}
