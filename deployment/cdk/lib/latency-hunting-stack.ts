import * as cdk from 'aws-cdk-lib';
import { 
  Vpc, 
  SubnetType, 
  Instance, 
  InstanceType,
  MachineImage, 
  SecurityGroup, 
  Port, 
  Peer, 
  BlockDeviceVolume,
  CfnPlacementGroup,
  UserData,
  KeyPair,
  CfnInstance
} from 'aws-cdk-lib/aws-ec2';
import { Tags, RemovalPolicy, CustomResource, Duration } from 'aws-cdk-lib';
import { Provider } from 'aws-cdk-lib/custom-resources';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import * as iam from 'aws-cdk-lib/aws-iam';

export interface LatencyHuntingStackProps extends cdk.StackProps {
  keyPairName?: string;
  maxInstancesPerType?: number;
  vpcCidr?: string;
  vpcId?: string;  // Use existing VPC instead of creating new one
}

export class LatencyHuntingStack extends cdk.Stack {
  constructor(scope: cdk.App, id: string, props?: LatencyHuntingStackProps) {
    super(scope, id, props);

    const keyPairName = props?.keyPairName || 'virginia';
    const maxInstancesPerType = props?.maxInstancesPerType || 1;
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
    
    if (vpcId) {
      // Import existing VPC
      console.log(`Using existing VPC: ${vpcId}`);
      vpc = Vpc.fromLookup(this, 'ExistingVpc', {
        vpcId: vpcId
      });
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

    // Get latest Amazon Linux 2023 AMI
    const ami = MachineImage.latestAmazonLinux2023();

    // Import existing key pair
    const keyPair = KeyPair.fromKeyPairName(this, 'ImportedKeyPair', keyPairName);

    // Create Lambda function for resilient instance creation
    const instanceCreatorLambda = new lambda.Function(this, 'InstanceCreatorLambda', {
      runtime: lambda.Runtime.PYTHON_3_11,
      handler: 'index.handler',
      timeout: Duration.minutes(5),
      code: lambda.Code.fromInline(`
import boto3
import json
import time
import random
import cfnresponse
from botocore.exceptions import ClientError

ec2 = boto3.client('ec2')

def handler(event, context):
    print(f"Event: {json.dumps(event)}")
    
    request_type = event['RequestType']
    
    if request_type == 'Delete':
        physical_resource_id = event.get('PhysicalResourceId')
        logical_resource_id = event.get('LogicalResourceId', '')
        stack_id = event.get('StackId', '')
        
        instances_to_terminate = []
        
        # Always search by CloudFormationLogicalId tag - this is the reliable method
        print(f"Searching for instances with CloudFormationLogicalId: {logical_resource_id}")
        
        try:
            # Search for instances with matching CloudFormationLogicalId tag
            filters = [
                {'Name': 'tag:CloudFormationLogicalId', 'Values': [logical_resource_id]},
                {'Name': 'tag:ManagedBy', 'Values': ['CDK-LatencyHunting']},
                {'Name': 'instance-state-name', 'Values': ['pending', 'running', 'stopping', 'stopped']}
            ]
            
            # Also filter by stack ID if available for extra safety
            if stack_id:
                filters.append({'Name': 'tag:CloudFormationStackId', 'Values': [stack_id]})
            
            response = ec2.describe_instances(Filters=filters)
            
            for reservation in response['Reservations']:
                for instance in reservation['Instances']:
                    instance_id = instance['InstanceId']
                    instances_to_terminate.append(instance_id)
                    print(f"Found instance by CloudFormationLogicalId tag: {instance_id}")
            
            if not instances_to_terminate:
                print(f"No instances found for LogicalResourceId: {logical_resource_id}")
                # Also check if PhysicalResourceId is a valid instance ID as fallback
                if physical_resource_id and physical_resource_id.startswith('i-'):
                    print(f"Checking PhysicalResourceId as fallback: {physical_resource_id}")
                    try:
                        check_response = ec2.describe_instances(InstanceIds=[physical_resource_id])
                        if check_response['Reservations']:
                            instances_to_terminate.append(physical_resource_id)
                            print(f"Found instance from PhysicalResourceId: {physical_resource_id}")
                    except ClientError as e:
                        if e.response['Error']['Code'] != 'InvalidInstanceID.NotFound':
                            print(f"Error checking PhysicalResourceId: {str(e)}")
                
        except Exception as e:
            print(f"Error searching for instances: {str(e)}")
        
        # Terminate all found instances
        for instance_id in instances_to_terminate:
            try:
                ec2.terminate_instances(InstanceIds=[instance_id])
                print(f"Initiated termination for instance: {instance_id}")
                
                # Wait for instance to be terminated or at least shutting down
                max_wait_time = 120  # 2 minutes should be enough for most instances
                start_time = time.time()
                
                while time.time() - start_time < max_wait_time:
                    try:
                        response = ec2.describe_instances(InstanceIds=[instance_id])
                        if response['Reservations']:
                            state = response['Reservations'][0]['Instances'][0]['State']['Name']
                            print(f"Instance {instance_id} state: {state}")
                            
                            if state == 'terminated':
                                print(f"Instance {instance_id} successfully terminated")
                                break
                            elif state == 'shutting-down':
                                # Instance is in shutting-down state, give it a bit more time
                                # but if we're running out of time, accept this state
                                if time.time() - start_time > 90:
                                    print(f"Instance {instance_id} is shutting-down, proceeding")
                                    break
                        else:
                            # No reservation found, instance is gone
                            print(f"Instance {instance_id} reservation not found (terminated)")
                            break
                    except ClientError as e:
                        if e.response['Error']['Code'] == 'InvalidInstanceID.NotFound':
                            print(f"Instance {instance_id} not found (terminated)")
                            break
                        raise
                    
                    time.sleep(5)  # Wait 5 seconds between checks
                
                if time.time() - start_time >= max_wait_time:
                    print(f"Warning: Timeout waiting for instance {instance_id} termination, but termination initiated")
                
            except ClientError as e:
                error_code = e.response['Error']['Code']
                if error_code == 'InvalidInstanceID.NotFound':
                    # Instance already terminated or doesn't exist
                    print(f"Instance {instance_id} not found (already terminated)")
                else:
                    print(f"Error terminating instance {instance_id}: {str(e)}")
                    # Don't fail - allow stack deletion to proceed
            except Exception as e:
                print(f"Unexpected error terminating instance {instance_id}: {str(e)}")
                # Don't fail - allow stack deletion to proceed
        
        if not instances_to_terminate:
            print(f"No instances to terminate for {logical_resource_id}")
            
        cfnresponse.send(event, context, cfnresponse.SUCCESS, {})
        return
    
    if request_type == 'Update':
        # On update, check if we need to replace the instance
        # If KeyName or other critical properties changed, we need to replace
        physical_resource_id = event.get('PhysicalResourceId', '')
        old_props = event.get('OldResourceProperties', {})
        new_props = event.get('ResourceProperties', {})
        
        # Check if this is a failed instance (PhysicalResourceId doesn't start with 'i-')
        # or if critical properties changed (KeyName, InstanceType, etc.)
        needs_replacement = (
            not physical_resource_id.startswith('i-') or
            old_props.get('KeyName') != new_props.get('KeyName') or
            old_props.get('InstanceType') != new_props.get('InstanceType') or
            old_props.get('PlacementGroup') != new_props.get('PlacementGroup')
        )
        
        if needs_replacement:
            # Terminate old instance if it exists
            if physical_resource_id.startswith('i-'):
                try:
                    ec2.terminate_instances(InstanceIds=[physical_resource_id])
                    print(f"Terminated old instance: {physical_resource_id}")
                    # Wait a bit for termination to start
                    time.sleep(5)
                except Exception as e:
                    print(f"Error terminating old instance: {str(e)}")
            
            # Continue to create a new instance (fall through to Create logic below)
            request_type = 'Create'
        else:
            # No replacement needed, just return success
            cfnresponse.send(event, context, cfnresponse.SUCCESS, {}, physical_resource_id)
            return
    
    # Create request - wrap everything in try-except to guarantee proper response
    props = event['ResourceProperties']
    instance_type = props.get('InstanceType', 'unknown')
    
    try:
        subnet_id = props['SubnetId']
        security_group_id = props['SecurityGroupId']
        key_name = props['KeyName']
        placement_group = props.get('PlacementGroup', '')
        
        # Add random delay to spread out API calls and avoid rate limiting
        # With 50+ instance types, this spreads launches over ~5 minutes
        delay = random.uniform(0, 5)
        print(f"Adding {delay:.2f}s delay to avoid API rate limiting")
        time.sleep(delay)
        
        # Detect instance architecture
        print(f"Detecting architecture for instance type: {instance_type}")
        instance_types_info = ec2.describe_instance_types(
            InstanceTypes=[instance_type]
        )
        
        architecture = instance_types_info['InstanceTypes'][0]['ProcessorInfo']['SupportedArchitectures'][0]
        print(f"Instance type {instance_type} architecture: {architecture}")
        
        # Get appropriate AMI based on architecture
        ami_filters = [
            {'Name': 'name', 'Values': ['al2023-ami-*']},
            {'Name': 'state', 'Values': ['available']},
            {'Name': 'architecture', 'Values': [architecture]},
            {'Name': 'owner-alias', 'Values': ['amazon']}
        ]
        
        images = ec2.describe_images(Filters=ami_filters, Owners=['amazon'])
        
        if not images['Images']:
            raise Exception(f"No AMI found for architecture {architecture}")
        
        # Sort by creation date and get the latest
        sorted_images = sorted(images['Images'], key=lambda x: x['CreationDate'], reverse=True)
        ami_id = sorted_images[0]['ImageId']
        print(f"Selected AMI: {ami_id} for architecture {architecture}")
        
        # Try to launch instance
        # UserData script to install Java and trading benchmark client
        user_data_script = '''#!/bin/bash
set -e

# Log everything to file
exec > >(tee /var/log/user-data.log)
exec 2>&1

echo "Starting EC2 Hunting setup at $(date)"

# Install Java 17
echo "Installing Java 17..."
dnf install -y java-17-amazon-corretto wget unzip

# Create directory for benchmark
mkdir -p /home/ec2-user/benchmark
cd /home/ec2-user/benchmark

# Download the trading benchmark JAR
echo "Downloading trading benchmark JAR..."
wget -O ExchangeFlow-v1.0.0.jar https://github.com/aws-samples/trading-latency-benchmark/releases/download/v1.0.0/ExchangeFlow-v1.0.0.jar

# Download config files
echo "Downloading config files..."
wget -O config-samples-v1.0.0.zip https://github.com/aws-samples/trading-latency-benchmark/releases/download/v1.0.0/config-samples-v1.0.0.zip

# Extract config
unzip -q config-samples-v1.0.0.zip

# Find and rename config file
CONFIG_FILE=$(find . -name "*config.properties" | head -n 1)
if [ -n "$CONFIG_FILE" ]; then
    cp "$CONFIG_FILE" config.properties
    echo "Config file found and copied to config.properties"
else
    echo "ERROR: Config file not found in zip!"
    exit 1
fi

# Update config parameters
# These will be replaced by actual values when tests run
echo "Configuring benchmark parameters..."
sed -i 's/^WARMUP_COUNT=.*/WARMUP_COUNT=1/' config.properties
sed -i 's/^TEST_SIZE=.*/TEST_SIZE=100/' config.properties

# Set ownership
chown -R ec2-user:ec2-user /home/ec2-user/benchmark

# Create ready marker
touch /home/ec2-user/benchmark/setup_complete

echo "EC2 Hunting setup completed successfully at $(date)"
'''
        
        run_params = {
            'ImageId': ami_id,
            'InstanceType': instance_type,
            'MinCount': 1,
            'MaxCount': 1,
            'KeyName': key_name,
            'NetworkInterfaces': [{
                'DeviceIndex': 0,
                'SubnetId': subnet_id,
                'Groups': [security_group_id],
                'AssociatePublicIpAddress': True
            }],
            'BlockDeviceMappings': [{
                'DeviceName': '/dev/xvda',
                'Ebs': {
                    'VolumeSize': 30,
                    'VolumeType': 'gp3',
                    'DeleteOnTermination': True
                }
            }],
            'UserData': user_data_script,
            'TagSpecifications': [{
                'ResourceType': 'instance',
                'Tags': [
                    {'Key': 'Name', 'Value': f'Latency-Hunting-Probe-{instance_type}'},
                    {'Key': 'Role', 'Value': 'hunting-probe'},
                    {'Key': 'InstanceType', 'Value': instance_type},
                    {'Key': 'Architecture', 'Value': 'latency-hunting'},
                    {'Key': 'ManagedBy', 'Value': 'CDK-LatencyHunting'},
                    {'Key': 'CloudFormationLogicalId', 'Value': event.get('LogicalResourceId', 'unknown')},
                    {'Key': 'CloudFormationStackId', 'Value': event.get('StackId', 'unknown')}
                ]
            }]
        }
        
        if placement_group:
            run_params['Placement'] = {'GroupName': placement_group}
            run_params['TagSpecifications'][0]['Tags'].append(
                {'Key': 'PlacementGroup', 'Value': placement_group}
            )
        
        # Run instances with exponential backoff retry for rate limiting
        max_retries = 5
        for attempt in range(max_retries):
            try:
                response = ec2.run_instances(**run_params)
                instance_id = response['Instances'][0]['InstanceId']
                print(f"Instance launched successfully on attempt {attempt + 1}")
                break
            except ClientError as e:
                if 'RequestLimitExceeded' in str(e) and attempt < max_retries - 1:
                    # Exponential backoff with jitter
                    backoff_time = (2 ** attempt) + random.uniform(0, 1)
                    print(f"Rate limited, retrying in {backoff_time:.2f}s (attempt {attempt + 1}/{max_retries})")
                    time.sleep(backoff_time)
                else:
                    # Either not a throttling error, or we've exhausted retries
                    raise
        
        # Wait for instance to have a public IP
        max_retries = 30
        for i in range(max_retries):
            try:
                instances = ec2.describe_instances(InstanceIds=[instance_id])
                public_ip = instances['Reservations'][0]['Instances'][0].get('PublicIpAddress')
                if public_ip:
                    break
            except:
                pass
            time.sleep(2)
        
        print(f"Successfully launched instance: {instance_id} of type {instance_type}")
        
        cfnresponse.send(event, context, cfnresponse.SUCCESS, {
            'InstanceId': instance_id,
            'InstanceType': instance_type,
            'Success': 'true',
            'PublicIp': public_ip or 'pending'
        }, instance_id)
        
    except Exception as e:
        error_msg = str(e)
        print(f"Failed to launch instance type {instance_type}: {error_msg}")
        
        # List of error patterns that should be treated as graceful failures
        # These allow deployment to continue even if specific instance types fail
        graceful_error_patterns = [
            'InsufficientInstanceCapacity',  # No capacity available
            'Unsupported',                    # Feature not supported
            'InvalidInstanceType',            # Instance type doesn't exist
            'does not exist',                 # Instance type not in region
            'is not supported',               # Not supported in region/AZ
            'InstanceLimitExceeded',          # Hit service limits
            'VcpuLimitExceeded',              # Hit vCPU limits
            'RequestLimitExceeded',           # API rate limit exceeded
            'InvalidParameterCombination',    # Invalid parameter combination (e.g., CPG + T2/T4g)
            'not supported for instances',    # Feature incompatibility
            'not available',                  # Generally not available
        ]
        
        # Check if this is a graceful failure
        is_graceful_failure = any(pattern in error_msg for pattern in graceful_error_patterns)
        
        if is_graceful_failure:
            # Return success but indicate failure in data
            # This allows deployment to continue for other instance types
            print(f"Treating as graceful failure: {error_msg}")
            cfnresponse.send(event, context, cfnresponse.SUCCESS, {
                'InstanceId': 'failed',
                'InstanceType': instance_type,
                'Success': 'false',
                'Error': error_msg
            }, f'failed-{instance_type}')
        else:
            # Real error that should fail the resource
            print(f"Treating as hard failure: {error_msg}")
            cfnresponse.send(event, context, cfnresponse.FAILED, {
                'Error': error_msg,
                'InstanceId': 'failed',
                'InstanceType': instance_type,
                'Success': 'false'
            })
    
    except Exception as unexpected_error:
        # Failsafe: Catch any unexpected errors from outer scope
        # This prevents CloudFormation from complaining about missing InstanceId attribute
        error_msg = str(unexpected_error)
        print(f"Unexpected outer error in Lambda handler: {error_msg}")
        cfnresponse.send(event, context, cfnresponse.SUCCESS, {
            'InstanceId': 'failed',
            'InstanceType': instance_type,
            'Success': 'false',
            'Error': f'Unexpected error: {error_msg}'
        }, f'failed-{instance_type}')
`),
    });

    // Grant Lambda permissions to manage EC2 instances and query AMIs
    instanceCreatorLambda.addToRolePolicy(new iam.PolicyStatement({
      actions: [
        'ec2:RunInstances',
        'ec2:TerminateInstances',
        'ec2:DescribeInstances',
        'ec2:DescribeInstanceTypes',
        'ec2:DescribeImages',
        'ec2:CreateTags'
      ],
      resources: ['*']
    }));

    // Create custom resource provider
    const provider = new Provider(this, 'InstanceCreatorProvider', {
      onEventHandler: instanceCreatorLambda,
    });

    // Track successfully created instances
    const successfulInstances: { instanceType: string; customResource: CustomResource }[] = [];

    // Create placement groups and instances for each instance type
    instanceTypes.forEach((instanceType, index) => {
      // Create placement group for this instance type
      const placementGroup = new CfnPlacementGroup(this, `PlacementGroup-${instanceType}`, {
        strategy: 'cluster'
      });
      placementGroup.applyRemovalPolicy(RemovalPolicy.DESTROY);

      // Get first public subnet (works for both created and imported VPCs)
      const publicSubnets = vpc.publicSubnets;
      if (publicSubnets.length === 0) {
        throw new Error('No public subnets found in VPC');
      }
      
      // Create instance using custom resource for resilience
      // Note: AMI is selected by Lambda based on instance architecture
      const instanceResource = new CustomResource(this, `Instance-${instanceType}`, {
        serviceToken: provider.serviceToken,
        properties: {
          InstanceType: instanceType,
          SubnetId: publicSubnets[0].subnetId,
          SecurityGroupId: securityGroup.securityGroupId,
          KeyName: keyPairName,
          PlacementGroup: placementGroup.ref,
          // Add timestamp to force updates if needed
          Timestamp: Date.now().toString()
        }
      });

      instanceResource.node.addDependency(placementGroup);

      successfulInstances.push({
        instanceType,
        customResource: instanceResource
      });

      // Note: Individual outputs removed to avoid attribute errors
      // Query instances via AWS CLI using tags: Name=tag:Architecture,Values=latency-hunting
    });

    // Output summary
    new cdk.CfnOutput(this, 'TotalInstanceTypes', {
      value: instanceTypes.length.toString(),
      description: 'Total number of instance types attempted'
    });

    new cdk.CfnOutput(this, 'VpcId', {
      value: vpc.vpcId,
      description: 'VPC ID used for hunting instances'
    });

    new cdk.CfnOutput(this, 'Region', {
      value: this.region,
      description: 'Deployment region'
    });
  }
}
