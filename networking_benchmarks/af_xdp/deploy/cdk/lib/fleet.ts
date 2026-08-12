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
  UserData,
  KeyPair,
} from 'aws-cdk-lib/aws-ec2';
import { Tags, RemovalPolicy } from 'aws-cdk-lib';

// DEFAULT is c7i.4xlarge = 16 vCPU / 8 physical cores (nosmt -> 8 online).
// MULTICAST: each node runs a single busy-poll app so it needs only 3
// dedicated cores (OS, ENA IRQ, app). UNICAST is heavier: the sender co-locates
// the replicator poll thread AND the rtt sender AND the rtt receiver, and each
// wants its own physical core alongside OS + a SEPARATE ENA-IRQ core (keeping the
// IRQ off the poll core avoids the tail jitter that hit --xdp-tx) = 5 cores.
// One AMI serves both: core pinning is derived
// dynamically at runtime from the isolated set (bake-ami.sh isolcpus, the
// replicator's initializeCpuCores, and run_ucast.yaml auto_pin), so it adapts to
// whatever instance a scenario deploys — 4xlarge -> metal.
const DEFAULT_INSTANCE_TYPE = 'c7i.4xlarge';
const DEFAULT_ROLE = 'destination';
const DEFAULT_PRIMARY_CIDR = '10.61.0.0/16';
const DEFAULT_SECONDARY_CIDR = '10.62.0.0/16';
const CONTROL_PORT = 12345;

export type PlacementStrategy = 'cluster' | 'spread' | 'partition';

/** A single node in the fleet specification. */
export interface FleetEntry {
  /** EC2 instance type. Default: c7i.4xlarge */
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
  /** This stack's region (explicit — env.region may be a token at synth). */
  regionName: string;
  /** Peer region VPC CIDR — opens SG ingress for cross-region data/control. */
  peerVpcCidr?: string;
  /** Resolve the AMI from SSM /af-xdp/ami/<region> (primary region only). */
  ssmAmi?: boolean;
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

/** Validate placement-group constraints for one region's entries. */
function validateEntries(entries: ResolvedEntry[]): void {
  const clusterGroupAZs = new Map<string, Set<string>>();
  const spreadPerAz = new Map<string, number>();

  for (const entry of entries) {
    const count = entry.count ?? 1;
    const az = entry._resolvedAz;
    if (entry.pgType === 'cluster') {
      const group = entry.pgName ?? '__default__';
      if (!clusterGroupAZs.has(group)) clusterGroupAZs.set(group, new Set());
      clusterGroupAZs.get(group)!.add(az);
    }
    if (entry.pgType === 'spread') {
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
    sg.addIngressRule(Peer.anyIpv4(), Port.tcp(22), 'SSH');
    sg.addIngressRule(sg, Port.allTraffic(), 'All intra-group traffic');
    if (props.peerVpcCidr) {
      sg.addIngressRule(Peer.ipv4(props.peerVpcCidr), Port.udp(dataPort), 'UDP data from peer region');
      sg.addIngressRule(Peer.ipv4(props.peerVpcCidr), Port.udp(CONTROL_PORT), 'UDP control from peer region');
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

    // ── AMI + key pair ─────────────────────────────────────────────────────
    // Primary region resolves the baked AMI from SSM (/af-xdp/ami/<region>,
    // written by ami-builder). A secondary region can't read the primary's SSM
    // param at deploy time, so it uses an explicit amiId or falls back to AL2023
    // (bake/provision it separately). Override either with --context amiId=.
    let ami: ec2.IMachineImage;
    if (props.amiId) {
      ami = MachineImage.genericLinux({ [region]: props.amiId });
    } else if (props.ssmAmi) {
      const ssmParam = ssm.StringParameter.valueForStringParameter(this, `/af-xdp/ami/${region}`);
      ami = MachineImage.genericLinux({ [region]: ssmParam });
    } else {
      ami = MachineImage.latestAmazonLinux2023();
    }
    const keyPair = KeyPair.fromKeyPairName(this, 'KeyPair', props.keyPairName);

    // ── Instances ──────────────────────────────────────────────────────────
    const fleetManifest: {
      index: number; instanceType: string; role: string;
      az: string; region: string; pgType: string | null; pgName: string | null; outputPrefix: string;
    }[] = [];

    let globalIndex = 0;
    for (const entry of props.entries) {
      const count = entry.count ?? 1;
      const role = entry.role ?? DEFAULT_ROLE;
      const instType = entry.type ?? DEFAULT_INSTANCE_TYPE;
      const az = entry._resolvedAz;
      const pgType = entry.pgType ?? null;

      for (let i = 0; i < count; i++) {
        const shortType = instType.replace('.', '-');
        const nodeId = `Node${globalIndex}`;
        const nodeName = `${role}-${globalIndex}-${shortType}`;
        const prefix = `Node${globalIndex}`;

        // Per-node agent config: stamp the role (known here) so the baked
        // control-plane agent knows its role. (AWS::EC2::Instance can't enable
        // IMDS instance-tags without a launch template, so we set it directly.)
        const agentUd = UserData.forLinux();
        agentUd.addCommands(
          `echo "AGENT_ROLE=${role}" >> /etc/default/afxdp-agent`,
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
        // Let the agent preflight read the control-plane NATS URL from SSM.
        inst.role.addToPrincipalPolicy(new iam.PolicyStatement({
          actions: ['ssm:GetParameter'],
          resources: [cdk.Arn.format({ service: 'ssm', resource: 'parameter', resourceName: 'af-xdp/*' }, this)],
        }));
        inst.role.addToPrincipalPolicy(new iam.PolicyStatement({
          actions: ['ec2:DescribeInstanceTypes'],
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

        new cdk.CfnOutput(this, `${prefix}InstanceId`, { value: inst.instanceId });
        new cdk.CfnOutput(this, `${prefix}PublicIp`, { value: inst.instancePublicIp });
        new cdk.CfnOutput(this, `${prefix}PrivateIp`, { value: inst.instancePrivateIp });

        fleetManifest.push({
          index: globalIndex, instanceType: instType, role, az,
          region, pgType, pgName: entry.pgName ?? null, outputPrefix: prefix,
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
