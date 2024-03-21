import * as cdk from 'aws-cdk-lib';
import { Vpc, SubnetType, Instance, InstanceType, InstanceClass, InstanceSize, MachineImage, SecurityGroup, Port, Peer, AmazonLinuxEdition, AmazonLinuxVirt, AmazonLinuxStorage, BlockDeviceVolume } from 'aws-cdk-lib/aws-ec2';

export class LowLatencyConnectStack extends cdk.Stack {
  constructor(scope: cdk.App, id: string, props?: cdk.StackProps) {
    super(scope, id, props);

    const testVpc = new Vpc(this, 'LowLatencyConnectVPC', {
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
    });

    const securityGroup = new SecurityGroup(this, 'InstanceSecurityGroup', {
      vpc: testVpc,
      description: 'Allow SSH access',
      allowAllOutbound: true,
    });

    securityGroup.addIngressRule(
      Peer.anyIpv4(),
      Port.tcp(22),
      'Allow SSH access from the public internet'
    );


    const ami = MachineImage.latestAmazonLinux2({
      edition: AmazonLinuxEdition.STANDARD,
      virtualization: AmazonLinuxVirt.HVM,
      storage: AmazonLinuxStorage.EBS,
    });
    const availabilityZones = testVpc.availabilityZones

    for (let i = 0; i < availabilityZones.length; i++) {
      const availabilityZone = availabilityZones[i];
      new Instance(this, `r4-xlarge-${availabilityZone}`, {
        vpc: testVpc,
        instanceType: InstanceType.of(InstanceClass.R4, InstanceSize.XLARGE),
        machineImage: ami,
        securityGroup,
        vpcSubnets:  {
          availabilityZones: [availabilityZone]
        },
        keyName: 'virginia',
        blockDevices: [{
          deviceName: "/dev/xvda",
          volume: BlockDeviceVolume.ebs(100)
        }]
      });
    }
  }
}
