import * as cdk from 'aws-cdk-lib';
import { Vpc, SubnetType, Instance, InstanceType, MachineImage, SecurityGroup, Port, Peer, BlockDeviceVolume, KeyPair, AmazonLinuxCpuType } from 'aws-cdk-lib/aws-ec2';
import { RemovalPolicy } from 'aws-cdk-lib';

export interface TradingBenchmarkAmiBuilderStackProps extends cdk.StackProps {
    instanceType?: string;
    baseAmi?: string;
    keyPairName?: string;
}

/**
 * CDK Stack for building an OS-tuned AMI for trading applications.
 * Deploys a single EC2 instance that will be tuned and converted to an AMI.
 */
export class TradingBenchmarkAmiBuilderStack extends cdk.Stack {
    public readonly instance: Instance;
    public readonly instanceId: cdk.CfnOutput;
    public readonly instancePublicIp: cdk.CfnOutput;

    constructor(scope: cdk.App, id: string, props?: TradingBenchmarkAmiBuilderStackProps) {
        super(scope, id, props);

        // Default instance type if not provided - use a common type for AMI building
        const instanceType = props?.instanceType || 'c7i.4xlarge';

        // Create a minimal VPC for AMI building
        const vpc = new Vpc(this, 'AmiBuilderVPC', {
            natGateways: 0, // No NAT gateway needed - use public subnet
            maxAzs: 1, // Use only first AZ in the region
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

        // Apply removal policy to make cleanup easier
        vpc.applyRemovalPolicy(RemovalPolicy.DESTROY);

        // Security group allowing SSH access
        const securityGroup = new SecurityGroup(this, 'AmiBuilderSecurityGroup', {
            vpc,
            description: 'Allow SSH access for AMI builder instance',
            allowAllOutbound: true,
        });

        securityGroup.applyRemovalPolicy(RemovalPolicy.DESTROY);

        securityGroup.addIngressRule(
            Peer.anyIpv4(),
            Port.tcp(22),
            'Allow SSH access for configuration'
        );

        // Import existing key pair
        const keyPairName = props?.keyPairName || 'virginia';
        const keyPair = KeyPair.fromKeyPairName(this, 'ImportedKeyPair', keyPairName);

        // Parse instance type
        let instanceTypeObj: InstanceType;
        try {
            instanceTypeObj = new InstanceType(instanceType);
        } catch (error) {
            console.warn(`Could not parse instance type ${instanceType}, using default: c7i.4xlarge`);
            instanceTypeObj = new InstanceType('c7i.4xlarge');
        }

        // Detect architecture from instance type and select matching AMI
        // ARM64 instance types: c8g, c7g, c6g, m8g, m7g, m6g, r8g, r7g, r6g, etc.
        const isArm64 = instanceType.match(/^[a-z][0-9]+g[a-z]?\./i) !== null;
        
        // Use base AMI if specified, otherwise use latest Amazon Linux 2023 with matching architecture
        const ami = props?.baseAmi
            ? MachineImage.genericLinux({ [this.region]: props.baseAmi })
            : MachineImage.latestAmazonLinux2023({
                cpuType: isArm64 ? AmazonLinuxCpuType.ARM_64 : AmazonLinuxCpuType.X86_64
            });
        
        console.log(`Instance type: ${instanceType}, Detected architecture: ${isArm64 ? 'ARM64' : 'x86_64'}`);

        // Create single instance for AMI building
        this.instance = new Instance(this, 'AmiBuilderInstance', {
            vpc,
            instanceType: instanceTypeObj,
            machineImage: ami,
            securityGroup,
            vpcSubnets: {
                subnetType: SubnetType.PUBLIC
            },
            keyPair,
            blockDevices: [{
                deviceName: "/dev/xvda",
                volume: BlockDeviceVolume.ebs(30, {
                    deleteOnTermination: true
                })
            }]
        });

        // Apply removal policy
        this.instance.applyRemovalPolicy(RemovalPolicy.DESTROY);

        // Add tags to identify the AMI builder instance
        cdk.Tags.of(this.instance).add('Name', 'Trading-Benchmark-AMI-Builder-Instance');
        cdk.Tags.of(this.instance).add('Role', 'ami-builder');
        cdk.Tags.of(this.instance).add('Purpose', 'os-tuned-ami');

        // Output the instance ID and public IP for use by orchestration script
        this.instanceId = new cdk.CfnOutput(this, 'InstanceId', {
            value: this.instance.instanceId,
            description: 'AMI Builder Instance ID',
            exportName: 'AmiBuilderInstanceId'
        });

        this.instancePublicIp = new cdk.CfnOutput(this, 'InstancePublicIp', {
            value: this.instance.instancePublicIp,
            description: 'AMI Builder Instance Public IP',
            exportName: 'AmiBuilderInstancePublicIp'
        });
    }
}
