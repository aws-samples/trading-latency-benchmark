import * as cdk from 'aws-cdk-lib';
import * as ec2 from 'aws-cdk-lib/aws-ec2';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import * as logs from 'aws-cdk-lib/aws-logs';
import { Tags, RemovalPolicy, Duration, CustomResource } from 'aws-cdk-lib';
import * as fs from 'fs';
import * as path from 'path';
import * as zlib from 'zlib';

export interface AmiBuilderStackProps extends cdk.StackProps {
  keyPairName: string;
  instanceType?: string;
  gitRepo?: string;
  gitRef?: string;
}

export class AmiBuilderStack extends cdk.Stack {
  public readonly amiId: string;

  constructor(scope: cdk.App, id: string, props: AmiBuilderStackProps) {
    super(scope, id, props);

    // Builder instance type only affects bake time, not the resulting binary's
    // portability - the Makefile targets -march=x86-64-v3 (not -march=native),
    // so a binary built here runs correctly on both Intel and AMD fleet nodes
    // regardless of which vendor this builder happens to be.
    const instanceType = props.instanceType ?? 'm8a.2xlarge';
    const gitRepo = props.gitRepo ?? 'https://github.com/aws-samples/trading-latency-benchmark.git';
    const gitRef = props.gitRef ?? 'main';

    // ── VPC ──────────────────────────────────────────────────────────────────
    const vpc = new ec2.Vpc(this, 'Vpc', {
      natGateways: 0,
      maxAzs: 1,
      subnetConfiguration: [{
        cidrMask: 24,
        name: 'Public',
        subnetType: ec2.SubnetType.PUBLIC,
        mapPublicIpOnLaunch: true,
      }],
    });

    // ── Security Group ───────────────────────────────────────────────────────
    const sg = new ec2.SecurityGroup(this, 'Sg', {
      vpc,
      description: 'AMI builder: SSH debug + outbound',
      allowAllOutbound: true,
    });
    sg.addIngressRule(ec2.Peer.anyIpv4(), ec2.Port.tcp(22), 'SSH debug');

    // ── IAM Role ─────────────────────────────────────────────────────────────
    const role = new iam.Role(this, 'Role', {
      assumedBy: new iam.ServicePrincipal('ec2.amazonaws.com'),
      managedPolicies: [
        iam.ManagedPolicy.fromAwsManagedPolicyName('AmazonSSMManagedInstanceCore'),
      ],
    });
    role.addToPolicy(new iam.PolicyStatement({
      actions: ['ec2:CreateImage', 'ec2:DescribeImages', 'ec2:CreateTags', 'ec2:StopInstances'],
      resources: ['*'],
    }));
    role.addToPolicy(new iam.PolicyStatement({
      actions: ['logs:CreateLogGroup', 'logs:CreateLogStream', 'logs:PutLogEvents'],
      resources: [cdk.Arn.format({ service: 'logs', resource: 'log-group', resourceName: '/af-xdp/ami-builder:*' }, this)],
    }));

    // ── Wait Condition Handle ────────────────────────────────────────────────
    const waitHandle = new cdk.CfnWaitConditionHandle(this, 'WaitHandle');

    // ── UserData ─────────────────────────────────────────────────────────────
    const bakeScript = fs.readFileSync(path.resolve(__dirname, '..', 'scripts', 'bake-ami.sh'), 'utf-8');
    const envBlock = [
      `export STACK_NAME="${this.stackName}"`,
      `export REGION="${this.region}"`,
      `export GIT_REPO="${gitRepo}"`,
      `export GIT_REF="${gitRef}"`,
    ].join('\n');
    const fullScript = bakeScript.replace(
      'set -uo pipefail',
      `${envBlock}\nexport WAIT_HANDLE_URL="\$1"\nset -uo pipefail`
    );
    // EC2 UserData is capped at 16 KB. The bake script (with configs, systemd
    // units, CPU-isolation block) exceeds that once base64-encoded, so gzip it
    // and gunzip on the builder — compresses ~16KB of shell to ~5KB.
    const bakeScriptGz = zlib.gzipSync(Buffer.from(fullScript)).toString('base64');

    const userData = ec2.UserData.forLinux();
    userData.addCommands(
      `echo '${bakeScriptGz}' | base64 -d | gunzip > /tmp/bake-ami.sh`,
      `chmod +x /tmp/bake-ami.sh`,
      `/tmp/bake-ami.sh "${waitHandle.ref}" || true`,
    );

    // ── Builder Instance ─────────────────────────────────────────────────────
    const instance = new ec2.Instance(this, 'Builder', {
      vpc,
      instanceType: new ec2.InstanceType(instanceType),
      machineImage: ec2.MachineImage.latestAmazonLinux2023(),
      securityGroup: sg,
      role,
      keyPair: ec2.KeyPair.fromKeyPairName(this, 'KeyPair', props.keyPairName),
      blockDevices: [{ deviceName: '/dev/xvda', volume: ec2.BlockDeviceVolume.ebs(30) }],
      userData,
    });
    instance.applyRemovalPolicy(RemovalPolicy.DESTROY);
    Tags.of(instance).add('Name', 'af-xdp-ami-builder');

    // ── Wait Condition ───────────────────────────────────────────────────────
    const waitCondition = new cdk.CfnWaitCondition(this, 'WaitCondition', {
      handle: waitHandle.ref,
      timeout: '1200',
      count: 1,
    });
    waitCondition.node.addDependency(instance);

    // ── Lambda: Create AMI + terminate builder ───────────────────────────────
    const createAmiFunction = new lambda.Function(this, 'CreateAmiFn', {
      runtime: lambda.Runtime.PYTHON_3_12,
      handler: 'index.handler',
      timeout: Duration.minutes(15),
      logRetention: logs.RetentionDays.ONE_MONTH,
      code: lambda.Code.fromInline(`
import boto3
import cfnresponse
import time

def handler(event, context):
    if event['RequestType'] == 'Delete':
        cfnresponse.send(event, context, cfnresponse.SUCCESS, {})
        return

    ec2 = boto3.client('ec2')
    instance_id = event['ResourceProperties']['InstanceId']
    stack_name = event['ResourceProperties']['StackName']
    ami_id = None

    try:
        ts = time.strftime('%Y%m%d-%H%M%S')
        ami_name = f'af-xdp-bench-{ts}'
        print(f'Creating AMI {ami_name} from {instance_id}')
        resp = ec2.create_image(
            InstanceId=instance_id,
            Name=ami_name,
            Description=f'AF_XDP benchmark AMI built by {stack_name}',
            NoReboot=False,
            TagSpecifications=[{
                'ResourceType': 'image',
                'Tags': [
                    {'Key': 'Name', 'Value': ami_name},
                    {'Key': 'Builder', 'Value': stack_name},
                    {'Key': 'Source', 'Value': 'af-xdp-ami-builder'},
                ]
            }]
        )
        ami_id = resp['ImageId']
        print(f'AMI creation initiated: {ami_id}')

        waiter = ec2.get_waiter('image_available')
        waiter.wait(ImageIds=[ami_id], WaiterConfig={'Delay': 15, 'MaxAttempts': 40})
        print(f'AMI available: {ami_id}')

        region = event['ResourceProperties'].get('Region', boto3.session.Session().region_name)
        ssm = boto3.client('ssm')
        ssm.put_parameter(Name=f'/af-xdp/ami/{region}', Value=ami_id, Type='String', Overwrite=True)
        print(f'SSM: /af-xdp/ami/{region} = {ami_id}')

        ec2.terminate_instances(InstanceIds=[instance_id])
        print(f'Builder {instance_id} terminated')
        cfnresponse.send(event, context, cfnresponse.SUCCESS, {'AmiId': ami_id}, ami_id)

    except Exception as e:
        print(f'ERROR: {e}')
        try:
            ec2.terminate_instances(InstanceIds=[instance_id])
        except Exception:
            pass
        if ami_id:
            try:
                ec2.deregister_image(ImageId=ami_id)
            except Exception:
                pass
        cfnresponse.send(event, context, cfnresponse.FAILED, {'Error': str(e)})
`),
    });
    createAmiFunction.addToRolePolicy(new iam.PolicyStatement({
      actions: [
        'ec2:CreateImage', 'ec2:DescribeImages', 'ec2:CreateTags',
        'ec2:TerminateInstances', 'ec2:DescribeInstances',
        'ec2:DeregisterImage', 'ssm:PutParameter',
      ],
      resources: ['*'],
    }));

    // ── Custom Resource: trigger AMI creation after bake ─────────────────────
    const amiResource = new CustomResource(this, 'AmiResource', {
      serviceToken: createAmiFunction.functionArn,
      properties: {
        InstanceId: instance.instanceId,
        StackName: this.stackName,
        Region: this.region,
        Timestamp: Date.now().toString(),
      },
    });
    amiResource.node.addDependency(waitCondition);

    // ── Outputs ──────────────────────────────────────────────────────────────
    this.amiId = amiResource.getAttString('AmiId');
    new cdk.CfnOutput(this, 'AmiId', {
      value: this.amiId,
      description: 'Baked AMI ID',
    });
  }
}
