import * as cdk from 'aws-cdk-lib';
import { Vpc, SubnetType, Instance, InstanceType, InstanceClass, InstanceSize, MachineImage, SecurityGroup, Port, Peer, BlockDeviceVolume, KeyPair } from 'aws-cdk-lib/aws-ec2';
import { Tags, RemovalPolicy } from 'aws-cdk-lib';

export interface TradingBenchmarkMultiAzStackProps extends cdk.StackProps {
  instanceType?: string;
}

export class TradingBenchmarkMultiAzStack extends cdk.Stack {
  constructor(scope: cdk.App, id: string, props?: TradingBenchmarkMultiAzStackProps) {
    super(scope, id, props);

    // Default instance type if not provided
    const instanceTypeStr = props?.instanceType || 'r4.xlarge';

    const benchmarkVpc = new Vpc(this, 'TradingBenchmarkMultiAzVPC', {
      natGateways: 1,
      availabilityZones: ['us-east-1a','us-east-1b','us-east-1c','us-east-1d','us-east-1e','us-east-1f'],
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

    const securityGroup = new SecurityGroup(this, 'MultiAzBenchmarkSecurityGroup', {
      vpc: benchmarkVpc,
      description: 'Allow SSH access and internal traffic for multi-AZ trading benchmark',
      allowAllOutbound: true,
    });

    // Apply removal policy to security group
    securityGroup.applyRemovalPolicy(RemovalPolicy.DESTROY);

    securityGroup.addIngressRule(
      Peer.anyIpv4(),
      Port.tcp(22),
      'Allow SSH access from the public internet'
    );

    // Allow all traffic between instances in the security group
    securityGroup.addIngressRule(
      securityGroup,
      Port.allTraffic(),
      'Allow all traffic between instances in the security group'
    );

    const ami = MachineImage.latestAmazonLinux2023();
    const availabilityZones = benchmarkVpc.availabilityZones;

    // Import existing key pair
    const keyPair = KeyPair.fromKeyPairName(this, 'ImportedKeyPair', 'virginia');

    // Parse instance type
    let instanceType: InstanceType;
    try {
      instanceType = this.parseInstanceTypeString(instanceTypeStr);
    } catch (error) {
      console.warn(`Could not parse instance type ${instanceTypeStr}, using default: r4.xlarge`);
      instanceType = InstanceType.of(InstanceClass.R4, InstanceSize.XLARGE);
    }

    for (let i = 0; i < availabilityZones.length; i++) {
      const availabilityZone = availabilityZones[i];
      const instance = new Instance(this, `instance-${availabilityZone}`, {
        vpc: benchmarkVpc,
        instanceType,
        machineImage: ami,
        securityGroup,
        vpcSubnets:  {
          availabilityZones: [availabilityZone]
        },
        keyPair,
        blockDevices: [{
          deviceName: "/dev/xvda",
          volume: BlockDeviceVolume.ebs(100)
        }]
      });
      
      // Apply removal policy to instance
      instance.applyRemovalPolicy(RemovalPolicy.DESTROY);
      
      // Add tags to identify instances
      Tags.of(instance).add('Name', `Trading-Benchmark-MultiAZ-${availabilityZone}`);
      Tags.of(instance).add('Role', 'benchmark');
      Tags.of(instance).add('AZ', availabilityZone);
      
      // Output the instance IDs and public IPs
      new cdk.CfnOutput(this, `InstanceId-${availabilityZone}`, {
        value: instance.instanceId,
        description: `Instance ID in ${availabilityZone}`
      });

      new cdk.CfnOutput(this, `PublicIp-${availabilityZone}`, {
        value: instance.instancePublicIp,
        description: `Public IP in ${availabilityZone}`
      });
    }
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
