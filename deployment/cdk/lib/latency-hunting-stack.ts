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
import { BaseLatencyHuntingStack, BaseLatencyHuntingStackProps } from './base-latency-hunting-stack';

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

    // Define diverse, low-cost instance types to maximize network spine coverage
    // Includes current and previous generation instances across Intel, AMD, and ARM architectures
    // Excludes: GPU/accelerated, very large memory (U/X/z1d), HPC, Mac, and metal instances
    const instanceTypes = [
      // ===== CURRENT GENERATION - INTEL x86 =====
      
      // C family - Compute optimized Intel
      'c8i.xlarge',      // 8th gen Intel (latest)
      'c7i.xlarge',      // 7th gen Intel
      'c6i.xlarge',      // 6th gen Intel
      'c6in.xlarge',     // 6th gen Intel network optimized (100 Gbps)
      'c5.xlarge',       // 5th gen Intel
      'c5n.xlarge',      // 5th gen Intel network optimized (100 Gbps)
      'c5d.xlarge',      // 5th gen Intel with local NVMe
      
      // M family - General purpose Intel
      'm8i.xlarge',      // 8th gen Intel (latest)
      'm7i.xlarge',      // 7th gen Intel
      'm6i.xlarge',      // 6th gen Intel
      'm6in.xlarge',     // 6th gen Intel network optimized
      'm5.xlarge',       // 5th gen Intel
      'm5n.xlarge',      // 5th gen Intel network optimized
      'm5d.xlarge',      // 5th gen Intel with local NVMe
      'm5dn.xlarge',     // 5th gen Intel NVMe + network optimized
      
      // R family - Memory optimized Intel
      'r8i.xlarge',      // 8th gen Intel memory (latest)
      'r7i.xlarge',      // 7th gen Intel memory
      'r7iz.xlarge',     // 7th gen Intel memory high freq
      'r6i.xlarge',      // 6th gen Intel memory
      'r6in.xlarge',     // 6th gen Intel memory network optimized
      'r5.xlarge',       // 5th gen Intel memory
      'r5n.xlarge',      // 5th gen Intel memory network optimized
      'r5d.xlarge',      // 5th gen Intel memory with NVMe
      'r5dn.xlarge',     // 5th gen Intel memory NVMe + network
      
      // I family - Storage optimized Intel
      'i4i.xlarge',      // 4th gen Intel storage (NVMe)
      'i3.xlarge',       // 3rd gen Intel storage (NVMe)
      'i3en.xlarge',     // 3rd gen Intel storage enhanced networking
      
      // D family - Dense storage Intel
      'd3.xlarge',       // 3rd gen dense storage
      'd3en.xlarge',     // 3rd gen dense storage enhanced networking
      
      // T family - Burstable Intel
      't3.xlarge',       // 3rd gen Intel burstable (CPG supported)
      
      // ===== CURRENT GENERATION - AMD x86 =====
      
      // C family - Compute optimized AMD
      'c8a.xlarge',      // 8th gen AMD (latest)
      'c7a.xlarge',      // 7th gen AMD
      'c6a.xlarge',      // 6th gen AMD
      'c5a.xlarge',      // 5th gen AMD
      'c5ad.xlarge',     // 5th gen AMD with local NVMe
      
      // M family - General purpose AMD
      'm8a.xlarge',      // 8th gen AMD (latest)
      'm7a.xlarge',      // 7th gen AMD
      'm6a.xlarge',      // 6th gen AMD
      'm5a.xlarge',      // 5th gen AMD
      'm5ad.xlarge',     // 5th gen AMD with local NVMe
      
      // R family - Memory optimized AMD
      'r8a.xlarge',      // 8th gen AMD memory (latest)
      'r7a.xlarge',      // 7th gen AMD memory
      'r6a.xlarge',      // 6th gen AMD memory
      'r5a.xlarge',      // 5th gen AMD memory
      'r5ad.xlarge',     // 5th gen AMD memory with NVMe
      
      
      // ===== CURRENT GENERATION - AWS GRAVITON ARM =====
      
      // C family - Compute optimized Graviton
      'c8g.xlarge',      // 8th gen Graviton4 (latest ARM)
      'c7g.xlarge',      // 7th gen Graviton3
      'c7gn.xlarge',     // 7th gen Graviton3 network (200 Gbps!)
      'c6g.xlarge',      // 6th gen Graviton2
      'c6gd.xlarge',     // 6th gen Graviton2 with local NVMe
      
      // M family - General purpose Graviton
      'm8g.xlarge',      // 8th gen Graviton4 (latest ARM)
      'm7g.xlarge',      // 7th gen Graviton3
      'm6g.xlarge',      // 6th gen Graviton2
      'm6gd.xlarge',     // 6th gen Graviton2 with local NVMe
      
      // R family - Memory optimized Graviton
      'r8g.xlarge',      // 8th gen Graviton4 memory (latest ARM)
      'r7g.xlarge',      // 7th gen Graviton3 memory
      'r6g.xlarge',      // 6th gen Graviton2 memory
      'r6gd.xlarge',     // 6th gen Graviton2 memory with NVMe
      
      // I family - Storage optimized Graviton
      'i4g.xlarge',      // 4th gen Graviton storage (ARM + NVMe)
      'i8g.xlarge',      // 8th gen Graviton4 storage (latest)
      'im4gn.xlarge',    // 4th gen Graviton2 memory + storage
      
      // ===== PREVIOUS GENERATION (Still available, cost-effective) =====
      
      // M1 - Original general purpose (very old, cheap)
      'm1.large',        // 1st gen general purpose
      
      // M3 - General purpose (old)
      'm3.xlarge',       // 3rd gen general purpose
      
      // M4 - General purpose (prev gen)
      'm4.xlarge',       // 4th gen general purpose
      
      // C1 - Compute optimized (very old)
      'c1.xlarge',       // 1st gen compute
      
      // C3 - Compute optimized (old)
      'c3.xlarge',       // 3rd gen compute
      
      // C4 - Compute optimized (prev gen)
      'c4.xlarge',       // 4th gen compute
      
      // R3 - Memory optimized (old)
      'r3.xlarge',       // 3rd gen memory
      
      // R4 - Memory optimized (prev gen)
      'r4.xlarge',       // 4th gen memory
      
      // I2 - Storage optimized (old)
      'i2.xlarge',       // 2nd gen storage
      
    ];

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
      instanceTypes,
      vpc,
      securityGroup,
      keyPair,
      subnetId
    );

    // Add outputs using base class method
    this.addCommonOutputs(
      instanceTypes,
      vpc.vpcId,
      subnetId,
      securityGroup.securityGroupId
    );
  }
}
