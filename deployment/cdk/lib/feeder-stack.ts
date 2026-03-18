/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 */
import * as cdk from 'aws-cdk-lib';
import * as ec2 from 'aws-cdk-lib/aws-ec2';
import * as cr from 'aws-cdk-lib/custom-resources';
import {
    Vpc, SubnetType, Instance, InstanceType, MachineImage,
    SecurityGroup, Port, Peer, BlockDeviceVolume, KeyPair,
} from 'aws-cdk-lib/aws-ec2';
import { Tags, RemovalPolicy } from 'aws-cdk-lib';

// ── FeederStack ────────────────────────────────────────────────────────────────
// Deploys MockExchange + Feeder in the feeder region (default: eu-west-2 / London).
// Software installation, GRE tunnel, and OS tuning are handled by Ansible
// via deploy-feeder.sh — instances boot vanilla Amazon Linux 2023.
export interface FeederStackProps extends cdk.StackProps {
    /** EC2 key pair name for SSH access (required) */
    keyPairName: string;
    /** Feeder instance type (default: c7i.4xlarge) */
    feederInstanceType?: string;
    /** Mock exchange instance type (default: c7i.4xlarge) */
    exchangeInstanceType?: string;
    /** AMI ID to use for all instances in the feeder region (required — use build-tuned-ami.sh) */
    amiId: string;
    /** VPC CIDR (default: 10.61.0.0/16) */
    vpcCidr?: string;
    /** Inner multicast group address used as listen_ip on feeder (default: 224.0.31.50) */
    multicastGroup?: string;
    /** UDP data port (default: 5000) */
    dataPort?: number;
    /** UDP upstream control port (default: 5001) */
    ctrlPort?: number;
    /**
     * Source feeder public IP CIDR for the inbound data-port SG rule.
     * Defaults to 0.0.0.0/0 when the upstream IP is not known at deploy time.
     */
    sourceFeederCidr?: string;
}

export class FeederStack extends cdk.Stack {
    readonly vpc: Vpc;
    readonly sg: SecurityGroup;
    readonly vpcCidr: string;

    constructor(scope: cdk.App, id: string, props: FeederStackProps) {
        super(scope, id, props);

        const feederInstanceType   = props.feederInstanceType   || 'c7i.4xlarge';
        const exchangeInstanceType = props.exchangeInstanceType || 'c7i.4xlarge';
        const vpcCidr              = props.vpcCidr              || '10.61.0.0/16';
        this.vpcCidr = vpcCidr;
        const multicastGroup       = props.multicastGroup       || '224.0.31.50';
        const dataPort             = props.dataPort             ?? 5000;
        const ctrlPort             = props.ctrlPort             ?? 5001;

        // ── VPC ────────────────────────────────────────────────────────────
        const vpc = new Vpc(this, 'FeederVpc', {
            ipAddresses: cdk.aws_ec2.IpAddresses.cidr(vpcCidr),
            natGateways: 0,
            maxAzs: 3,
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
        for (const subnet of vpc.publicSubnets) {
            if (subnet.node.defaultChild instanceof cdk.CfnResource) {
                (subnet.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
            }
        }

        // ── Security group ─────────────────────────────────────────────────
        // - SSH from internet
        // - Self-referencing all-traffic: GRE (proto 47), UDP fan-out, control, ICMP
        // - UDP dataPort from upstream source feeder (cross-region fan-out ingress)
        const sg = new SecurityGroup(this, 'FeederSg', {
            vpc,
            description: 'feeder POC: SSH + intra-cluster GRE, UDP, control',
            allowAllOutbound: true,
        });
        sg.applyRemovalPolicy(RemovalPolicy.DESTROY);
        this.sg = sg;
        sg.addIngressRule(Peer.anyIpv4(), Port.tcp(22), 'SSH from internet');
        sg.addIngressRule(
            sg,
            Port.allTraffic(),
            'All intra-cluster traffic: GRE (proto 47), UDP data and control, ICMP'
        );
        sg.addIngressRule(
            props.sourceFeederCidr ? Peer.ipv4(props.sourceFeederCidr) : Peer.anyIpv4(),
            Port.udp(dataPort),
            props.sourceFeederCidr
                ? `UDP data port from upstream source feeder (${props.sourceFeederCidr})`
                : 'UDP data port - open (set sourceFeederCidr to restrict to upstream feeder IP)'
        );

        const ami     = MachineImage.genericLinux({ [this.region]: props.amiId });
        const keyPair = KeyPair.fromKeyPairName(this, 'KeyPair', props.keyPairName);

        // ── Mock exchange instance ─────────────────────────────────────────
        const exchange = new Instance(this, 'Exchange', {
            vpc,
            vpcSubnets: { subnets: [vpc.publicSubnets[1]] },
            instanceType: new InstanceType(exchangeInstanceType),
            machineImage: ami,
            securityGroup: sg,
            keyPair,
            blockDevices: [{ deviceName: '/dev/xvda', volume: BlockDeviceVolume.ebs(40) }],
        });
        exchange.applyRemovalPolicy(RemovalPolicy.DESTROY);
        Tags.of(exchange).add('Name', 'MockExchange');
        Tags.of(exchange).add('Role', 'exchange');

        // ── Feeder instance ────────────────────────────────────────────────
        const feeder = new Instance(this, 'Feeder', {
            vpc,
            vpcSubnets: { subnets: [vpc.publicSubnets[1]] },
            instanceType: new InstanceType(feederInstanceType),
            machineImage: ami,
            securityGroup: sg,
            keyPair,
            blockDevices: [{ deviceName: '/dev/xvda', volume: BlockDeviceVolume.ebs(40) }],
        });
        feeder.applyRemovalPolicy(RemovalPolicy.DESTROY);
        Tags.of(feeder).add('Name', 'Feeder');
        Tags.of(feeder).add('Role', 'feeder');

        // ── Stack outputs ──────────────────────────────────────────────────
        new cdk.CfnOutput(this, 'FeederPublicIp', {
            value: feeder.instancePublicIp,
            description: 'Feeder public IP - SSH target and cross-region fan-out destination',
        });
        new cdk.CfnOutput(this, 'FeederPrivateIp', {
            value: feeder.instancePrivateIp,
            description: 'Feeder private IP - GRE remote endpoint and subscriber fan-out source',
        });
        new cdk.CfnOutput(this, 'ExchangePublicIp', {
            value: exchange.instancePublicIp,
            description: 'Mock exchange public IP',
        });
        new cdk.CfnOutput(this, 'ExchangePrivateIp', {
            value: exchange.instancePrivateIp,
            description: 'Mock exchange private IP',
        });

        // ── Runbook outputs ────────────────────────────────────────────────
        new cdk.CfnOutput(this, 'RunStep1_StartFeeder', {
            value: [
                `ssh ec2-user@<FeederPublicIp>`,
                `cd ~/trading-latency-benchmark/af_xdp_zero_copy_perf_benchmark`,
                `sudo ./packet_replicator eth0 ${multicastGroup} ${dataPort} true --gre`,
            ].join('  &&  '),
            description: 'Run: Start feeder in GRE mode',
        });
        new cdk.CfnOutput(this, 'RunStep1_StartFeeder_WithCtrl', {
            value: [
                `ssh ec2-user@<FeederPublicIp>`,
                `cd ~/trading-latency-benchmark/af_xdp_zero_copy_perf_benchmark`,
                `sudo ./packet_replicator eth0 ${multicastGroup} ${dataPort} true --gre`,
                `  --ctrl ${multicastGroup}:${ctrlPort} --producer ${exchange.instancePrivateIp}:6000`,
            ].join('  &&  '),
            description: 'Run (alt): Start feeder with upstream control forwarding',
        });
        new cdk.CfnOutput(this, 'RunStep3_SendTestTraffic', {
            value: [
                `ssh ec2-user@<ExchangePublicIp>`,
                `cd ~/trading-latency-benchmark/af_xdp_zero_copy_perf_benchmark`,
                `./test_client ${multicastGroup} ${dataPort} 10 "trade" --iface eth0`,
            ].join('  &&  '),
            description: 'Run: Send test traffic from mock exchange (GRE tunnel auto-configured by Ansible)',
        });
        new cdk.CfnOutput(this, 'RunStep3_Benchmark', {
            value: [
                `ssh ec2-user@<ExchangePublicIp>`,
                `cd ~/trading-latency-benchmark/af_xdp_zero_copy_perf_benchmark`,
                `./market_data_provider_client ${multicastGroup} ${dataPort}`,
                `  <subscriber_private_ip> 9001 1000000 10000 --feeder-ip ${feeder.instancePrivateIp}`,
            ].join('  '),
            description: 'Run (alt): Full RTT benchmark from mock exchange',
        });
    }
}

// ── SubscriberStack ────────────────────────────────────────────────────────────
// Deploys subscriber instances in the subscriber region (default: eu-central-1 / Frankfurt).
// Subscribers receive unicast UDP fan-out from the Feeder over the public internet.
export interface SubscriberStackProps extends cdk.StackProps {
    /** EC2 key pair name for SSH access (required) */
    keyPairName: string;
    /** AMI ID to use for subscriber instances in the subscriber region (required — use build-tuned-ami.sh) */
    amiId: string;
    /** Subscriber instance type (default: c7i.4xlarge) */
    subscriberInstanceType?: string;
    /** Number of subscriber instances (default: 2) */
    subscriberCount?: number;
    /** VPC CIDR (default: 10.62.0.0/16) */
    vpcCidr?: string;
    /** UDP data port (default: 5000) */
    dataPort?: number;
    /**
     * Feeder public IP CIDR for the inbound data-port SG rule.
     * Defaults to 0.0.0.0/0 when the feeder IP is not known at deploy time.
     * Set to <FeederPublicIp>/32 after the feeder stack is deployed.
     */
    feederCidr?: string;
}

export class SubscriberStack extends cdk.Stack {
    readonly vpc: Vpc;
    readonly sg: SecurityGroup;
    readonly vpcCidr: string;

    constructor(scope: cdk.App, id: string, props: SubscriberStackProps) {
        super(scope, id, props);

        const subscriberInstanceType = props.subscriberInstanceType || 'c7i.4xlarge';
        const subscriberCount        = props.subscriberCount        ?? 2;
        const vpcCidr                = props.vpcCidr                || '10.62.0.0/16';
        this.vpcCidr = vpcCidr;
        const dataPort               = props.dataPort               ?? 5000;

        // ── VPC ────────────────────────────────────────────────────────────
        const vpc = new Vpc(this, 'SubscriberVpc', {
            ipAddresses: cdk.aws_ec2.IpAddresses.cidr(vpcCidr),
            natGateways: 0,
            maxAzs: 3,
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
        for (const subnet of vpc.publicSubnets) {
            if (subnet.node.defaultChild instanceof cdk.CfnResource) {
                (subnet.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
            }
        }

        // ── Security group ─────────────────────────────────────────────────
        // - SSH from internet
        // - UDP dataPort from feeder public IP (cross-region fan-out)
        const sg = new SecurityGroup(this, 'SubscriberSg', {
            vpc,
            description: 'subscriber: SSH + UDP data port from feeder',
            allowAllOutbound: true,
        });
        sg.applyRemovalPolicy(RemovalPolicy.DESTROY);
        this.sg = sg;
        sg.addIngressRule(Peer.anyIpv4(), Port.tcp(22), 'SSH from internet');
        sg.addIngressRule(
            props.feederCidr ? Peer.ipv4(props.feederCidr) : Peer.anyIpv4(),
            Port.udp(dataPort),
            props.feederCidr
                ? `UDP data port from feeder (${props.feederCidr})`
                : 'UDP data port - open (set feederCidr to restrict to feeder public IP)'
        );

        const ami     = MachineImage.genericLinux({ [this.region]: props.amiId });
        const keyPair = KeyPair.fromKeyPairName(this, 'KeyPair', props.keyPairName);

        // ── Subscriber instances ───────────────────────────────────────────
        const subscribers: Instance[] = [];
        for (let i = 0; i < subscriberCount; i++) {
            const sub = new Instance(this, `Subscriber${i + 1}`, {
                vpc,
                vpcSubnets: { subnets: [vpc.publicSubnets[1]] },
                instanceType: new InstanceType(subscriberInstanceType),
                machineImage: ami,
                securityGroup: sg,
                keyPair,
                blockDevices: [{ deviceName: '/dev/xvda', volume: BlockDeviceVolume.ebs(40) }],
            });
            sub.applyRemovalPolicy(RemovalPolicy.DESTROY);
            Tags.of(sub).add('Name', `Subscriber-${i + 1}`);
            Tags.of(sub).add('Role', 'subscriber');
            subscribers.push(sub);
        }

        // ── Stack outputs ──────────────────────────────────────────────────
        for (let i = 0; i < subscribers.length; i++) {
            new cdk.CfnOutput(this, `Subscriber${i + 1}PublicIp`, {
                value: subscribers[i].instancePublicIp,
                description: `Subscriber ${i + 1} public IP`,
            });
            new cdk.CfnOutput(this, `Subscriber${i + 1}PrivateIp`, {
                value: subscribers[i].instancePrivateIp,
                description: `Subscriber ${i + 1} private IP`,
            });
        }
    }
}

// ── PeeringStack ───────────────────────────────────────────────────────────────
// Creates a cross-region VPC peering connection between the feeder VPC (requester)
// and the subscriber VPC (accepter). Same-account cross-region peering is
// auto-accepted by CloudFormation. Routes and SG rules are added on both sides.
//
// Deployed in the feeder region after FeederStack and SubscriberStack.
// AwsCustomResource handles all subscriber-side operations cross-region.
export interface PeeringStackProps extends cdk.StackProps {
    /** Feeder VPC (requester side, same region as this stack) */
    feederVpc: Vpc;
    /** Plain CIDR string for the feeder VPC, e.g. '10.61.0.0/16' */
    feederVpcCidr: string;
    /** Feeder security group — receives an ingress rule for subscriber CIDR */
    feederSg: SecurityGroup;
    /** CloudFormation stack name of the SubscriberStack, used for resource lookup */
    subscriberStackName: string;
    /** AWS region where SubscriberStack is deployed */
    subscriberRegion: string;
    /** Plain CIDR string for the subscriber VPC, e.g. '10.62.0.0/16' */
    subscriberVpcCidr: string;
    /** UDP data port opened through the peering (same as dataPort on both stacks) */
    dataPort: number;
}

export class PeeringStack extends cdk.Stack {
    constructor(scope: cdk.App, id: string, props: PeeringStackProps) {
        super(scope, id, props);

        // ── Look up subscriber VPC ID ──────────────────────────────────────
        // CDK tags every VPC with aws:cloudformation:stack-name at creation.
        const subVpcLookup = new cr.AwsCustomResource(this, 'SubVpcLookup', {
            onUpdate: {
                service: 'EC2',
                action: 'DescribeVpcs',
                parameters: {
                    Filters: [{ Name: 'tag:aws:cloudformation:stack-name', Values: [props.subscriberStackName] }],
                },
                region: props.subscriberRegion,
                physicalResourceId: cr.PhysicalResourceId.of(`sub-vpc-${props.subscriberStackName}`),
                outputPaths: ['Vpcs.0.VpcId'],
            },
            policy: cr.AwsCustomResourcePolicy.fromSdkCalls({ resources: cr.AwsCustomResourcePolicy.ANY_RESOURCE }),
        });
        const subscriberVpcId = subVpcLookup.getResponseField('Vpcs.0.VpcId');

        // ── Look up subscriber route table ─────────────────────────────────
        // CDK creates one route table per public subnet; take the first one.
        const subRtLookup = new cr.AwsCustomResource(this, 'SubRtLookup', {
            onUpdate: {
                service: 'EC2',
                action: 'DescribeRouteTables',
                parameters: {
                    Filters: [{ Name: 'tag:aws:cloudformation:stack-name', Values: [props.subscriberStackName] }],
                },
                region: props.subscriberRegion,
                physicalResourceId: cr.PhysicalResourceId.of(`sub-rt-${props.subscriberStackName}`),
                outputPaths: ['RouteTables.0.RouteTableId'],
            },
            policy: cr.AwsCustomResourcePolicy.fromSdkCalls({ resources: cr.AwsCustomResourcePolicy.ANY_RESOURCE }),
        });
        const subscriberRouteTableId = subRtLookup.getResponseField('RouteTables.0.RouteTableId');

        // ── Look up subscriber security group ──────────────────────────────
        const subSgLookup = new cr.AwsCustomResource(this, 'SubSgLookup', {
            onUpdate: {
                service: 'EC2',
                action: 'DescribeSecurityGroups',
                parameters: {
                    Filters: [
                        { Name: 'vpc-id', Values: [subscriberVpcId] },
                        { Name: 'description', Values: ['subscriber: SSH + UDP data port from feeder'] },
                    ],
                },
                region: props.subscriberRegion,
                physicalResourceId: cr.PhysicalResourceId.of(`sub-sg-${props.subscriberStackName}`),
                outputPaths: ['SecurityGroups.0.GroupId'],
            },
            policy: cr.AwsCustomResourcePolicy.fromSdkCalls({ resources: cr.AwsCustomResourcePolicy.ANY_RESOURCE }),
        });
        subSgLookup.node.addDependency(subVpcLookup);
        const subscriberSgId = subSgLookup.getResponseField('SecurityGroups.0.GroupId');

        // ── VPC Peering Connection (requester: feeder, accepter: subscriber) ──
        // Same-account cross-region peering is auto-accepted by CloudFormation.
        const peering = new ec2.CfnVPCPeeringConnection(this, 'VpcPeering', {
            vpcId: props.feederVpc.vpcId,
            peerVpcId: subscriberVpcId,
            peerRegion: props.subscriberRegion,
            tags: [{ key: 'Name', value: 'FeederToSubscriberPeering' }],
        });
        peering.node.addDependency(subVpcLookup);

        // ── Feeder-side routes → subscriber CIDR ──────────────────────────
        for (const subnet of props.feederVpc.publicSubnets) {
            new ec2.CfnRoute(this, `FeederRoute${subnet.node.id}`, {
                routeTableId: subnet.routeTable.routeTableId,
                destinationCidrBlock: props.subscriberVpcCidr,
                vpcPeeringConnectionId: peering.ref,
            });
        }

        // ── Feeder SG: allow inbound UDP from subscriber VPC ───────────────
        new ec2.CfnSecurityGroupIngress(this, 'FeederSgFromSubscriber', {
            groupId: props.feederSg.securityGroupId,
            ipProtocol: 'udp',
            fromPort: props.dataPort,
            toPort: props.dataPort,
            cidrIp: props.subscriberVpcCidr,
            description: `UDP data port from subscriber VPC via peering`,
        });

        // ── Subscriber-side route → feeder CIDR (cross-region) ────────────
        const addSubRoute = new cr.AwsCustomResource(this, 'AddSubscriberRoute', {
            onCreate: {
                service: 'EC2',
                action: 'CreateRoute',
                parameters: {
                    RouteTableId:           subscriberRouteTableId,
                    DestinationCidrBlock:   props.feederVpcCidr,
                    VpcPeeringConnectionId: peering.ref,
                },
                region: props.subscriberRegion,
                physicalResourceId: cr.PhysicalResourceId.of(`sub-route-${props.subscriberStackName}`),
                ignoreErrorCodesMatching: 'RouteAlreadyExists',
            },
            onDelete: {
                service: 'EC2',
                action: 'DeleteRoute',
                parameters: {
                    RouteTableId:         subscriberRouteTableId,
                    DestinationCidrBlock: props.feederVpcCidr,
                },
                region: props.subscriberRegion,
                ignoreErrorCodesMatching: 'InvalidRoute.NotFound',
            },
            policy: cr.AwsCustomResourcePolicy.fromSdkCalls({ resources: cr.AwsCustomResourcePolicy.ANY_RESOURCE }),
        });
        addSubRoute.node.addDependency(peering);
        addSubRoute.node.addDependency(subRtLookup);

        // ── Subscriber SG: allow inbound UDP from feeder VPC (cross-region) ─
        const addSubSgRule = new cr.AwsCustomResource(this, 'AddSubscriberSgRule', {
            onCreate: {
                service: 'EC2',
                action: 'AuthorizeSecurityGroupIngress',
                parameters: {
                    GroupId: subscriberSgId,
                    IpPermissions: [{
                        IpProtocol: 'udp',
                        FromPort: props.dataPort,
                        ToPort: props.dataPort,
                        IpRanges: [{ CidrIp: props.feederVpcCidr, Description: 'UDP from feeder VPC via peering' }],
                    }],
                },
                region: props.subscriberRegion,
                physicalResourceId: cr.PhysicalResourceId.of(`sub-sg-rule-${props.subscriberStackName}`),
                ignoreErrorCodesMatching: 'InvalidPermission.Duplicate',
            },
            onDelete: {
                service: 'EC2',
                action: 'RevokeSecurityGroupIngress',
                parameters: {
                    GroupId: subscriberSgId,
                    IpPermissions: [{
                        IpProtocol: 'udp',
                        FromPort: props.dataPort,
                        ToPort: props.dataPort,
                        IpRanges: [{ CidrIp: props.feederVpcCidr }],
                    }],
                },
                region: props.subscriberRegion,
                ignoreErrorCodesMatching: 'InvalidPermission.NotFound',
            },
            policy: cr.AwsCustomResourcePolicy.fromSdkCalls({ resources: cr.AwsCustomResourcePolicy.ANY_RESOURCE }),
        });
        addSubSgRule.node.addDependency(subSgLookup);

        new cdk.CfnOutput(this, 'PeeringConnectionId', {
            value: peering.ref,
            description: 'VPC peering connection ID (FeederStack ↔ SubscriberStack)',
        });
    }
}
