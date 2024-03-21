import * as cdk from 'aws-cdk-lib';
import {Template} from 'aws-cdk-lib/assertions';
import * as LowLatencyConnect from '../lib/low-latency-connect-stack';

// Test: VPC is created
test('VPC Created', () => {
    const app = new cdk.App();
    const stack = new LowLatencyConnect.LowLatencyConnectStack(app, 'MyTestStack');
    const template = Template.fromStack(stack);

    template.resourceCountIs('AWS::EC2::VPC', 1);
});

// Test: Security Group is created with the correct ingress rule so that we can SSH into EC2s
test('Security Group Created and Allows SSH', () => {
    const app = new cdk.App();
    const stack = new LowLatencyConnect.LowLatencyConnectStack(app, 'MyTestStack');
    const template = Template.fromStack(stack);

    const resources = template.findResources('AWS::EC2::SecurityGroup', {
        Properties: {
            GroupDescription: 'Allow SSH access',
            SecurityGroupEgress: [
                {
                    Description: 'Allow all outbound traffic by default',
                    IpProtocol: '-1',
                    CidrIp: '0.0.0.0/0',
                },
            ],
        },
    });

    expect(Object.keys(resources)).toHaveLength(1);

    //Make sure Security group allow inbound SSH connections
    const securityGroup =  Object.entries(resources)[0][1]
    const ingressRule = securityGroup.Properties.SecurityGroupIngress[0];
    expect(ingressRule.CidrIp).toEqual('0.0.0.0/0');
    expect(ingressRule.FromPort).toEqual(22);
    expect(ingressRule.ToPort).toEqual(22);
    expect(ingressRule.IpProtocol).toEqual('tcp');
});

// Test: 6 EC2 instances are created
test('6 EC2 Instances Created', () => {
    const app = new cdk.App();
    const stack = new LowLatencyConnect.LowLatencyConnectStack(app, 'MyTestStack');
    const template = Template.fromStack(stack);

    const instances = Object.entries(template.findResources('AWS::EC2::Instance'));

    expect(instances.length).toEqual(6); // One instance per Availability Zone
});

// Test 7: One EC2 instance per AZ
test('There exist EC2 instance per AZ', () => {
    const app = new cdk.App();
    const stack = new LowLatencyConnect.LowLatencyConnectStack(app, 'MyTestStack');
    const template = Template.fromStack(stack);

    for (let az of ["us-east-1a", "us-east-1b", "us-east-1c", "us-east-1d", "us-east-1e", "us-east-1f"])
        template.hasResourceProperties('AWS::EC2::Instance', {
            AvailabilityZone: az
            }
        );

});
