import * as cdk from 'aws-cdk-lib';
import { Vpc, SubnetType, Instance, InstanceType, InstanceClass, InstanceSize, MachineImage, SecurityGroup, Port, Peer, AmazonLinuxEdition, AmazonLinuxVirt, AmazonLinuxStorage, BlockDeviceVolume } from 'aws-cdk-lib/aws-ec2';

export class SingleMetalInstanceStack extends cdk.Stack {
    constructor(scope: cdk.App, id: string, props?: cdk.StackProps) {
        super(scope, id, props);

        const testVpc = new Vpc(this, 'LowLatencyConnectVPC', {
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

        const ami = MachineImage.genericLinux( {'us-east-1':'AMI_ID'});

        new Instance(this, 'c7i-metal-24xl', {
            vpc: testVpc,
            instanceType: InstanceType.of(InstanceClass.C7I, InstanceSize.XLARGE24METAL),
            machineImage: ami,
            securityGroup,
            vpcSubnets: {
                availabilityZones: ['us-east-1a']
            },
            keyName: 'virginia',
            blockDevices: [{
                deviceName: "/dev/xvda",
                volume: BlockDeviceVolume.ebs(512)
            }]
        });
    }
}
