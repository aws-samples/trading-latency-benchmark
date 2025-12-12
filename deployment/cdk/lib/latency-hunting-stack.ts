import * as cdk from 'aws-cdk-lib';
import { 
  Vpc, 
  SubnetType, 
  SecurityGroup, 
  Port, 
  Peer,
  KeyPair
} from 'aws-cdk-lib/aws-ec2';
import { RemovalPolicy } from 'aws-cdk-lib';
import { BaseLatencyHuntingStack, BaseLatencyHuntingStackProps, InstanceConfig } from './base-latency-hunting-stack';

export interface LatencyHuntingStackProps extends BaseLatencyHuntingStackProps {
  keyPairName?: string;
  vpcCidr?: string;
  vpcId?: string;  // Use existing VPC instead of creating new one
}

/**
 * Latency hunting stack that creates a new VPC or uses an existing one.
 * For BYOVPC scenarios, consider using LatencyHuntingBYOVPCStack instead.
 */
export class LatencyHuntingStack extends BaseLatencyHuntingStack {
  constructor(scope: cdk.App, id: string, props?: LatencyHuntingStackProps) {
    super(scope, id, {
      ...props,
      managedByTag: 'CDK-LatencyHunting'
    });

    const keyPairName = props?.keyPairName || 'virginia';
    const vpcCidr = props?.vpcCidr || '10.100.0.0/16';  // Default non-standard CIDR
    const vpcId = props?.vpcId;

    // Get instances from overridden method
    const instances = this.getDefaultInstances();

    // Use existing VPC or create new one
    let vpc: cdk.aws_ec2.IVpc;
    let subnetId: string;
    
    if (vpcId) {
      // Import existing VPC
      console.log(`Using existing VPC: ${vpcId}`);
      vpc = Vpc.fromLookup(this, 'ExistingVpc', {
        vpcId: vpcId
      });
      
      // Get first public subnet
      const publicSubnets = vpc.publicSubnets;
      if (publicSubnets.length === 0) {
        throw new Error('No public subnets found in VPC');
      }
      subnetId = publicSubnets[0].subnetId;
    } else {
      // Create new VPC with configurable CIDR to avoid overlaps
      console.log('Creating new VPC for latency hunting');
      const newVpc = new Vpc(this, 'LatencyHuntingVpc', {
        ipAddresses: cdk.aws_ec2.IpAddresses.cidr(vpcCidr),
        natGateways: 1,
        availabilityZones: [this.availabilityZones[0]], // Use first AZ
        subnetConfiguration: [
          {
            cidrMask: 24,
            name: 'Public',
            subnetType: SubnetType.PUBLIC,
            mapPublicIpOnLaunch: true
          }
        ],
        gatewayEndpoints: {}
      });

      newVpc.applyRemovalPolicy(RemovalPolicy.DESTROY);
      
      // Apply removal policy to all subnets
      for (const subnet of newVpc.publicSubnets) {
        if (subnet.node.defaultChild instanceof cdk.CfnResource) {
          (subnet.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
        }
      }

      // Apply removal policy to NAT gateways
      for (const natGateway of newVpc.node.findAll().filter(child => 
        child.node.defaultChild instanceof cdk.aws_ec2.CfnNatGateway)) {
        if (natGateway.node.defaultChild instanceof cdk.CfnResource) {
          (natGateway.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
        }
      }

      // Apply removal policy to route tables
      for (const routeTable of newVpc.node.findAll().filter(child => 
        child.node.defaultChild instanceof cdk.aws_ec2.CfnRouteTable)) {
        if (routeTable.node.defaultChild instanceof cdk.CfnResource) {
          (routeTable.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
        }
      }

      // Find and apply removal policy to GuardDuty data endpoint specifically
      const guardDutyEndpoints = newVpc.node.findAll().filter(child => 
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
      for (const endpoint of newVpc.node.findAll().filter(child => 
        child.node.defaultChild instanceof cdk.aws_ec2.CfnVPCEndpoint)) {
        if (endpoint.node.defaultChild instanceof cdk.CfnResource) {
          (endpoint.node.defaultChild as cdk.CfnResource).applyRemovalPolicy(RemovalPolicy.DESTROY);
        }
      }
      
      vpc = newVpc;
      subnetId = newVpc.publicSubnets[0].subnetId;
    }

    // Create security group
    const securityGroup = new SecurityGroup(this, 'LatencyHuntingSecurityGroup', {
      vpc,
      description: 'Security group for EC2 hunting deployment',
      allowAllOutbound: true,
    });

    securityGroup.applyRemovalPolicy(RemovalPolicy.DESTROY);

    // Allow SSH access
    securityGroup.addIngressRule(
      Peer.anyIpv4(),
      Port.tcp(22),
      'Allow SSH access'
    );

    // Allow all traffic between instances in the security group
    securityGroup.addIngressRule(
      securityGroup,
      Port.allTraffic(),
      'Allow all traffic within security group'
    );

    // Import existing key pair
    const keyPair = KeyPair.fromKeyPairName(this, 'ImportedKeyPair', keyPairName);

    // Create instances using base class method
    this.createInstances(
      instances,
      vpc,
      securityGroup,
      keyPair,
      subnetId
    );

    // Add outputs using base class method
    this.addCommonOutputs(
      instances.map(i => i.instanceType),
      vpc.vpcId,
      subnetId,
      securityGroup.securityGroupId
    );
  }
}
