import * as cdk from 'aws-cdk-lib';
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

export interface SingleRegionStackProps extends cdk.StackProps {
  /** SSH key pair name (must exist in the target region) */
  keyPairName: string;
  /** EC2 instance type for all three nodes. Default: c7i.4xlarge */
  instanceType?: string;
  /** Custom AMI ID. Default: latest Amazon Linux 2023 */
  amiId?: string;
  /** Number of subscriber instances. Default: 1 */
  subscriberCount?: number;
}

/**
 * Deploys three EC2 instances (exchange, feeder, subscriber) inside a single
 * Cluster Placement Group in a single-AZ public VPC.
 *
 * Stack inputs: region (via env.region), keyPairName.
 * Optional:     instanceType, amiId.
 *
 * Deploy:
 *   cd deployment/mcast_gre/cdk
 *   npm ci
 *   cdk deploy --context keyPairName=my-key --context region=eu-west-2
 */
export class SingleRegionStack extends cdk.Stack {
  constructor(scope: cdk.App, id: string, props: SingleRegionStackProps) {
    super(scope, id, props);

    const instanceTypeStr  = props.instanceType   ?? 'c7i.4xlarge';
    const subscriberCount  = props.subscriberCount ?? 1;
    // Use first AZ of the deployed region — avoids hardcoding a region-specific AZ.
    const az = this.availabilityZones[0];

    // ── VPC ──────────────────────────────────────────────────────────────────
    // Single public subnet, no NAT — all instances get public IPs directly.
    const vpc = new Vpc(this, 'Vpc', {
      natGateways: 0,
      availabilityZones: [az],
      subnetConfiguration: [
        {
          cidrMask: 24,
          name: 'Public',
          subnetType: SubnetType.PUBLIC,
          mapPublicIpOnLaunch: true,
        },
      ],
      gatewayEndpoints: {},
    });
    vpc.applyRemovalPolicy(RemovalPolicy.DESTROY);

    for (const subnet of vpc.publicSubnets) {
      if (subnet.node.defaultChild instanceof cdk.CfnResource) {
        (subnet.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
      }
    }
    for (const child of vpc.node.findAll()) {
      if (child.node.defaultChild instanceof cdk.aws_ec2.CfnRouteTable) {
        (child.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
      }
      if (child.node.defaultChild instanceof cdk.aws_ec2.CfnVPCEndpoint) {
        (child.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
      }
    }

    // ── Security group ────────────────────────────────────────────────────────
    const sg = new SecurityGroup(this, 'Sg', {
      vpc,
      description: 'CPG: SSH + intra-group traffic',
      allowAllOutbound: true,
    });
    sg.applyRemovalPolicy(RemovalPolicy.DESTROY);
    sg.addIngressRule(Peer.anyIpv4(), Port.tcp(22), 'SSH from internet');
    sg.addIngressRule(sg, Port.allTraffic(), 'All traffic within group');

    // ── Cluster Placement Group ───────────────────────────────────────────────
    const pg = new CfnPlacementGroup(this, 'PlacementGroup', {
      strategy: 'cluster',
    });
    pg.applyRemovalPolicy(RemovalPolicy.DESTROY);

    // ── AMI + key pair ────────────────────────────────────────────────────────
    const ami = props.amiId
      ? MachineImage.genericLinux({ [this.region]: props.amiId })
      : MachineImage.latestAmazonLinux2023();

    const keyPair = KeyPair.fromKeyPairName(this, 'KeyPair', props.keyPairName);
    const instanceType = new InstanceType(instanceTypeStr);
    const vpcSubnets = { availabilityZones: [az] };

    // ── Instances ─────────────────────────────────────────────────────────────
    const mkInstance = (id: string, name: string, role: string) => {
      const inst = new Instance(this, id, {
        vpc,
        instanceType,
        machineImage: ami,
        securityGroup: sg,
        vpcSubnets,
        keyPair,
        blockDevices: [{ deviceName: '/dev/xvda', volume: BlockDeviceVolume.ebs(100) }],
        userData: UserData.forLinux(),
      });
      inst.applyRemovalPolicy(RemovalPolicy.DESTROY);
      (inst.node.defaultChild as cdk.aws_ec2.CfnInstance).placementGroupName = pg.ref;
      Tags.of(inst).add('Name', name);
      Tags.of(inst).add('Role', role);
      Tags.of(inst).add('PlacementGroup', pg.ref);
      return inst;
    };

    const addOutputs = (inst: Instance, prefix: string, description: string) => {
      new cdk.CfnOutput(this, `${prefix}InstanceId`, { value: inst.instanceId,   description: `${description} instance ID` });
      new cdk.CfnOutput(this, `${prefix}PublicIp`,   { value: inst.instancePublicIp,  description: `${description} public IP` });
      new cdk.CfnOutput(this, `${prefix}PrivateIp`,  { value: inst.instancePrivateIp, description: `${description} private IP` });
    };

    addOutputs(mkInstance('ExchangeInstance', 'Trading-Exchange', 'exchange'), 'Exchange', 'Exchange');
    addOutputs(mkInstance('FeederInstance',   'Trading-Feeder',   'feeder'),   'Feeder',   'Feeder');

    for (let i = 1; i <= subscriberCount; i++) {
      addOutputs(
        mkInstance(`Subscriber${i}Instance`, `Trading-Subscriber-${i}`, 'subscriber'),
        `Subscriber${i}`,
        `Subscriber ${i}`,
      );
    }

    new cdk.CfnOutput(this, 'PlacementGroupName', {
      value: pg.ref,
      description: 'Cluster placement group name',
    });
    new cdk.CfnOutput(this, 'VpcId', {
      value: vpc.vpcId,
      description: 'VPC ID',
    });
    new cdk.CfnOutput(this, 'AvailabilityZone', {
      value: az,
      description: 'AZ all instances are placed in',
    });
  }
}
