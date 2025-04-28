import * as cdk from 'aws-cdk-lib';
import { Vpc, SubnetType, Instance, InstanceType, InstanceClass, InstanceSize, MachineImage, SecurityGroup, Port, Peer, BlockDeviceVolume, KeyPair, CfnVPCEndpoint } from 'aws-cdk-lib/aws-ec2';
import { Tags, RemovalPolicy } from 'aws-cdk-lib';

export interface TradingBenchmarkSingleInstanceStackProps extends cdk.StackProps {
    instanceType1?: string;
    instanceType2?: string;
}

export class TradingBenchmarkSingleInstanceStack extends cdk.Stack {
    constructor(scope: cdk.App, id: string, props?: TradingBenchmarkSingleInstanceStackProps) {
        super(scope, id, props);

        // Default instance types if not provided
        const instanceType1 = props?.instanceType1 || 'c7i.4xlarge';
        const instanceType2 = props?.instanceType2 || 'c6in.4xlarge';

        const benchmarkVpc = new Vpc(this, 'TradingBenchmarkVPC', {
            natGateways: 1,
            availabilityZones: ['us-east-1a'],
            subnetConfiguration: [
                {
                    cidrMask: 24,
                    name: 'Public',
                    subnetType: SubnetType.PUBLIC,
                    mapPublicIpOnLaunch: true
                }
            ],
            // Explicitly disable all gateway endpoints
            gatewayEndpoints: {}
        });

        // Apply removal policy to VPC
        benchmarkVpc.applyRemovalPolicy(RemovalPolicy.DESTROY);
        
        // Apply removal policy to all subnets
        for (const subnet of benchmarkVpc.publicSubnets) {
            if (subnet.node.defaultChild instanceof cdk.CfnResource) {
                (subnet.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
            }
        }

        // Apply removal policy to NAT gateways
        for (const natGateway of benchmarkVpc.node.findAll().filter(child => 
            child.node.defaultChild instanceof cdk.aws_ec2.CfnNatGateway)) {
            if (natGateway.node.defaultChild instanceof cdk.CfnResource) {
                (natGateway.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
            }
        }

        // Apply removal policy to route tables
        for (const routeTable of benchmarkVpc.node.findAll().filter(child => 
            child.node.defaultChild instanceof cdk.aws_ec2.CfnRouteTable)) {
            if (routeTable.node.defaultChild instanceof cdk.CfnResource) {
                (routeTable.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
            }
        }

        // Find and apply removal policy to GuardDuty data endpoint specifically
        const guardDutyEndpoints = benchmarkVpc.node.findAll().filter(child => 
            child.node.defaultChild instanceof cdk.aws_ec2.CfnVPCEndpoint && 
            (child.node.defaultChild as any).serviceName?.includes('guardduty-data')
        );

        for (const endpoint of guardDutyEndpoints) {
            if (endpoint.node.defaultChild instanceof cdk.CfnResource) {
                console.log('Applying removal policy to GuardDuty data endpoint');
                (endpoint.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
            }
        }

        // Apply removal policy to all VPC endpoints
        for (const endpoint of benchmarkVpc.node.findAll().filter(child => 
            child.node.defaultChild instanceof cdk.aws_ec2.CfnVPCEndpoint)) {
            if (endpoint.node.defaultChild instanceof cdk.CfnResource) {
                (endpoint.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
            }
        }

        const securityGroup = new SecurityGroup(this, 'BenchmarkSecurityGroup', {
            vpc: benchmarkVpc,
            description: 'Allow SSH access and internal traffic for trading benchmark',
            allowAllOutbound: true,
        });

        // Apply removal policy to security group
        securityGroup.applyRemovalPolicy(RemovalPolicy.DESTROY);

        securityGroup.addIngressRule(
            Peer.anyIpv4(),
            Port.tcp(22),
            'Allow SSH access from the public internet'
        );

        const ami = MachineImage.latestAmazonLinux2023();

        // Import existing key pair
        const keyPair = KeyPair.fromKeyPairName(this, 'ImportedKeyPair', 'virginia');

        // Parse instance types
        let instanceType1Obj: InstanceType;
        let instanceType2Obj: InstanceType;

        try {
            instanceType1Obj = this.parseInstanceTypeString(instanceType1);
        } catch (error) {
            console.warn(`Could not parse instance type ${instanceType1}, using default: c7i.4xlarge`);
            instanceType1Obj = InstanceType.of(InstanceClass.C7I, InstanceSize.XLARGE4);
        }

        try {
            instanceType2Obj = this.parseInstanceTypeString(instanceType2);
        } catch (error) {
            console.warn(`Could not parse instance type ${instanceType2}, using default: c6in.4xlarge`);
            instanceType2Obj = InstanceType.of(InstanceClass.C6IN, InstanceSize.XLARGE4);
        }

        // Create first instance
        const instance1 = new Instance(this, 'instance1', {
            vpc: benchmarkVpc,
            instanceType: instanceType1Obj,
            machineImage: ami,
            securityGroup,
            vpcSubnets: {
                availabilityZones: ['us-east-1a']
            },
            keyPair,
            blockDevices: [{
                deviceName: "/dev/xvda",
                volume: BlockDeviceVolume.ebs(512)
            }]
        });
        
        // Apply removal policy to instance
        instance1.applyRemovalPolicy(RemovalPolicy.DESTROY);
        
        // Create second instance
        const instance2 = new Instance(this, 'instance2', {
            vpc: benchmarkVpc,
            instanceType: instanceType2Obj,
            machineImage: ami,
            securityGroup,
            vpcSubnets: {
                availabilityZones: ['us-east-1a']
            },
            keyPair,
            blockDevices: [{
                deviceName: "/dev/xvda",
                volume: BlockDeviceVolume.ebs(512)
            }]
        });
        
        // Apply removal policy to instance
        instance2.applyRemovalPolicy(RemovalPolicy.DESTROY);

        // Add tags to identify instances
        Tags.of(instance1).add('Name', `Trading-Benchmark-${instanceType1}`);
        Tags.of(instance2).add('Name', `Trading-Benchmark-${instanceType2}`);
        Tags.of(instance1).add('Role', 'benchmark');
        Tags.of(instance2).add('Role', 'benchmark');
        
        // Output the instance IDs and public IPs
        new cdk.CfnOutput(this, 'Instance1Id', {
            value: instance1.instanceId,
            description: `Instance 1 (${instanceType1}) ID`
        });

        new cdk.CfnOutput(this, 'Instance1PublicIp', {
            value: instance1.instancePublicIp,
            description: `Instance 1 (${instanceType1}) Public IP`
        });
        
        new cdk.CfnOutput(this, 'Instance2Id', {
            value: instance2.instanceId,
            description: `Instance 2 (${instanceType2}) ID`
        });

        new cdk.CfnOutput(this, 'Instance2PublicIp', {
            value: instance2.instancePublicIp,
            description: `Instance 2 (${instanceType2}) Public IP`
        });
    }
    
    // Helper method to parse instance type string into InstanceType
    private parseInstanceTypeString(instanceTypeStr: string): InstanceType {
        const parts = instanceTypeStr.split('.');
        if (parts.length !== 2) {
            throw new Error(`Invalid instance type format: ${instanceTypeStr}`);
        }

        const classKey = parts[0].toLowerCase();
        const sizeKey = parts[1].toLowerCase();
        
        // Direct mapping to InstanceClass and InstanceSize enums
        let instanceClass: InstanceClass;
        let instanceSize: InstanceSize;
        
        // Map class
        switch (classKey) {
            case 'c7i': instanceClass = InstanceClass.C7I; break;
            case 'c6in': instanceClass = InstanceClass.C6IN; break;
            case 'c6i': instanceClass = InstanceClass.C6I; break;
            case 'c5': instanceClass = InstanceClass.C5; break;
            case 'r6i': instanceClass = InstanceClass.R6I; break;
            case 'r4': instanceClass = InstanceClass.R4; break;
            case 'm6i': instanceClass = InstanceClass.M6I; break;
            case 't3': instanceClass = InstanceClass.T3; break;
            default: throw new Error(`Unknown instance class: ${classKey}`);
        }
        
        // Map size
        switch (sizeKey) {
            case 'large': instanceSize = InstanceSize.LARGE; break;
            case 'xlarge': instanceSize = InstanceSize.XLARGE; break;
            case '2xlarge': instanceSize = InstanceSize.XLARGE2; break;
            case '4xlarge': instanceSize = InstanceSize.XLARGE4; break;
            case '8xlarge': instanceSize = InstanceSize.XLARGE8; break;
            case '16xlarge': instanceSize = InstanceSize.XLARGE16; break;
            case '24xlarge': instanceSize = InstanceSize.XLARGE24; break;
            case '32xlarge': instanceSize = InstanceSize.XLARGE32; break;
            default: throw new Error(`Unknown instance size: ${sizeKey}`);
        }

        return InstanceType.of(instanceClass, instanceSize);
    }
}
