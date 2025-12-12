import * as cdk from 'aws-cdk-lib';
import { 
  Vpc,
  SecurityGroup,
  ISecurityGroup,
  Port, 
  Peer,
  KeyPair
} from 'aws-cdk-lib/aws-ec2';
import { RemovalPolicy } from 'aws-cdk-lib';
import { BaseLatencyHuntingStack, BaseLatencyHuntingStackProps, InstanceConfig } from './base-latency-hunting-stack';

export interface LatencyHuntingBYOVPCStackProps extends BaseLatencyHuntingStackProps {
  vpcId: string;
  subnetId: string;
  keyPairName: string;
  securityGroupId?: string;
}

/**
 * Latency hunting stack that uses existing VPC resources (Bring Your Own VPC).
 * This is the recommended approach for production deployments.
 */
export class LatencyHuntingBYOVPCStack extends BaseLatencyHuntingStack {
  constructor(scope: cdk.App, id: string, props: LatencyHuntingBYOVPCStackProps) {
    super(scope, id, {
      ...props,
      managedByTag: 'CDK-BYOVPC'
    });

    // Validate required parameters
    if (!props.vpcId) {
      throw new Error('vpcId is required for BYOVPC stack');
    }
    if (!props.subnetId) {
      throw new Error('subnetId is required for BYOVPC stack');
    }
    if (!props.keyPairName) {
      throw new Error('keyPairName is required for BYOVPC stack');
    }

    // Use default instances from base class
    const instances = this.getDefaultInstances();

    // Import existing VPC
    const vpc = Vpc.fromLookup(this, 'ExistingVpc', {
      vpcId: props.vpcId
    });

    // Use existing security group or create minimal one
    let securityGroup: ISecurityGroup;
    
    if (props.securityGroupId) {
      // Import existing security group
      securityGroup = SecurityGroup.fromSecurityGroupId(
        this, 
        'ExistingSecurityGroup', 
        props.securityGroupId
      );
    } else {
      // Create minimal security group for instances only
      securityGroup = new SecurityGroup(this, 'HuntingInstancesSecurityGroup', {
        vpc,
        description: 'Security group for latency hunting instances (managed by CDK)',
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
    }

    // Import existing key pair
    const keyPair = KeyPair.fromKeyPairName(this, 'ImportedKeyPair', props.keyPairName);

    // Create instances using base class method
    this.createInstances(
      instances,
      vpc,
      securityGroup,
      keyPair,
      props.subnetId,
      this.elasticIps
    );

    // Add outputs using base class method
    this.addCommonOutputs(
      instances.map(i => i.instanceType),
      props.vpcId,
      props.subnetId,
      securityGroup.securityGroupId
    );
  }
}
