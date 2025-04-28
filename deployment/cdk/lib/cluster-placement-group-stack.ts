import * as cdk from 'aws-cdk-lib';
import { 
  Vpc, 
  SubnetType, 
  Instance, 
  InstanceType, 
  InstanceClass, 
  InstanceSize, 
  MachineImage, 
  SecurityGroup, 
  Port, 
  Peer, 
  BlockDeviceVolume,
  CfnPlacementGroup,
  UserData,
  KeyPair
} from 'aws-cdk-lib/aws-ec2';
import { Tags, RemovalPolicy } from 'aws-cdk-lib';

export interface TradingBenchmarkClusterPlacementGroupProps extends cdk.StackProps {
  clientInstanceType?: string;
  serverInstanceType?: string;
}

export class TradingBenchmarkClusterPlacementGroupStack extends cdk.Stack {
  constructor(scope: cdk.App, id: string, props?: TradingBenchmarkClusterPlacementGroupProps) {
    super(scope, id, props);

    // Default instance types if not provided
    const clientInstanceType = props?.clientInstanceType || 'c7i.4xlarge';
    const serverInstanceType = props?.serverInstanceType || 'c6in.4xlarge';

    // Create VPC with explicitly disabled endpoints
    const vpc = new Vpc(this, 'TradingBenchmarkClusterVPC', {
      natGateways: 1,
      availabilityZones: ['us-east-1a'], // Using a single AZ for placement group
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
    vpc.applyRemovalPolicy(RemovalPolicy.DESTROY);
    
    // Apply removal policy to all subnets
    for (const subnet of vpc.publicSubnets) {
      if (subnet.node.defaultChild instanceof cdk.CfnResource) {
        (subnet.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
      }
    }

    // Apply removal policy to NAT gateways
    for (const natGateway of vpc.node.findAll().filter(child => 
      child.node.defaultChild instanceof cdk.aws_ec2.CfnNatGateway)) {
      if (natGateway.node.defaultChild instanceof cdk.CfnResource) {
        (natGateway.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
      }
    }

    // Apply removal policy to route tables
    for (const routeTable of vpc.node.findAll().filter(child => 
      child.node.defaultChild instanceof cdk.aws_ec2.CfnRouteTable)) {
      if (routeTable.node.defaultChild instanceof cdk.CfnResource) {
        (routeTable.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
      }
    }

    // Find and apply removal policy to GuardDuty data endpoint specifically
    const guardDutyEndpoints = vpc.node.findAll().filter(child => 
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
    for (const endpoint of vpc.node.findAll().filter(child => 
      child.node.defaultChild instanceof cdk.aws_ec2.CfnVPCEndpoint)) {
      if (endpoint.node.defaultChild instanceof cdk.CfnResource) {
        (endpoint.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
      }
    }

    // Create security group
    const securityGroup = new SecurityGroup(this, 'ClusterPlacementGroupSecurityGroup', {
      vpc,
      description: 'Security group for trading benchmark cluster placement group',
      allowAllOutbound: true,
    });

    // Apply removal policy to security group
    securityGroup.applyRemovalPolicy(RemovalPolicy.DESTROY);

    // Allow SSH access
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

    // Create placement group
    const placementGroup = new CfnPlacementGroup(this, 'TradingBenchmarkPlacementGroup', {
      strategy: 'cluster'
    });

    // Apply removal policy to placement group
    placementGroup.applyRemovalPolicy(RemovalPolicy.DESTROY);

    // Get latest Amazon Linux 2023 AMI
    const ami = MachineImage.latestAmazonLinux2023();

    // Import existing key pair
    const keyPair = KeyPair.fromKeyPairName(this, 'ImportedKeyPair', 'virginia');

    // Parse instance types
    let clientInstanceTypeObj: InstanceType;
    let serverInstanceTypeObj: InstanceType;

    try {
      clientInstanceTypeObj = this.parseInstanceTypeString(clientInstanceType);
    } catch (error) {
      console.warn(`Could not parse client instance type ${clientInstanceType}, using default: c7i.4xlarge`);
      clientInstanceTypeObj = InstanceType.of(InstanceClass.C7I, InstanceSize.XLARGE4);
    }

    try {
      serverInstanceTypeObj = this.parseInstanceTypeString(serverInstanceType);
    } catch (error) {
      console.warn(`Could not parse server instance type ${serverInstanceType}, using default: c6in.4xlarge`);
      serverInstanceTypeObj = InstanceType.of(InstanceClass.C6IN, InstanceSize.XLARGE4);
    }

    // Create client instance
    const clientInstance = new Instance(this, 'TradingClientInstance', {
      vpc,
      instanceType: clientInstanceTypeObj,
      machineImage: ami,
      securityGroup,
      vpcSubnets: {
        availabilityZones: ['us-east-1a']
      },
      keyPair,
      blockDevices: [{
        deviceName: "/dev/xvda",
        volume: BlockDeviceVolume.ebs(512)
      }],
      userData: UserData.forLinux()
    });

    // Apply removal policy to client instance
    clientInstance.applyRemovalPolicy(RemovalPolicy.DESTROY);

    // Create server instance
    const serverInstance = new Instance(this, 'TradingServerInstance', {
      vpc,
      instanceType: serverInstanceTypeObj,
      machineImage: ami,
      securityGroup,
      vpcSubnets: {
        availabilityZones: ['us-east-1a']
      },
      keyPair,
      blockDevices: [{
        deviceName: "/dev/xvda",
        volume: BlockDeviceVolume.ebs(512)
      }],
      userData: UserData.forLinux()
    });

    // Apply removal policy to server instance
    serverInstance.applyRemovalPolicy(RemovalPolicy.DESTROY);

    // Add instances to placement group
    const cfnClientInstance = clientInstance.node.defaultChild as cdk.aws_ec2.CfnInstance;
    cfnClientInstance.placementGroupName = placementGroup.ref;

    const cfnServerInstance = serverInstance.node.defaultChild as cdk.aws_ec2.CfnInstance;
    cfnServerInstance.placementGroupName = placementGroup.ref;

    // Add tags to identify instances
    Tags.of(clientInstance).add('Role', 'client');
    Tags.of(serverInstance).add('Role', 'server');
    Tags.of(clientInstance).add('Name', 'Trading-Client');
    Tags.of(serverInstance).add('Name', 'Trading-Server');
    Tags.of(clientInstance).add('PlacementGroup', placementGroup.ref);
    Tags.of(serverInstance).add('PlacementGroup', placementGroup.ref);
    Tags.of(clientInstance).add('Architecture', 'cluster-placement-group');
    Tags.of(serverInstance).add('Architecture', 'cluster-placement-group');

    // Output the instance IDs and public IPs
    new cdk.CfnOutput(this, 'ClientInstanceId', {
      value: clientInstance.instanceId,
      description: 'Client Instance ID'
    });

    new cdk.CfnOutput(this, 'ClientPublicIp', {
      value: clientInstance.instancePublicIp,
      description: 'Client Public IP'
    });

    new cdk.CfnOutput(this, 'ServerInstanceId', {
      value: serverInstance.instanceId,
      description: 'Server Instance ID'
    });

    new cdk.CfnOutput(this, 'ServerPublicIp', {
      value: serverInstance.instancePublicIp,
      description: 'Server Public IP'
    });

    new cdk.CfnOutput(this, 'PlacementGroupName', {
      value: placementGroup.ref,
      description: 'Placement Group Name'
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
