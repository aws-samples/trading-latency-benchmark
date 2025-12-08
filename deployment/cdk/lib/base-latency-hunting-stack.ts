import * as cdk from 'aws-cdk-lib';
import { 
  IVpc,
  ISecurityGroup,
  CfnPlacementGroup,
  IKeyPair
} from 'aws-cdk-lib/aws-ec2';
import { RemovalPolicy, CustomResource, Duration } from 'aws-cdk-lib';
import { Provider } from 'aws-cdk-lib/custom-resources';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as dynamodb from 'aws-cdk-lib/aws-dynamodb';

export interface BaseLatencyHuntingStackProps extends cdk.StackProps {
  maxInstancesPerType?: number;
  managedByTag?: string;
}

/**
 * Base class for latency hunting stacks.
 * Implements the correct approach for creating instances across diverse instance types
 * with proper state tracking, resilient Lambda-based provisioning, and cleanup.
 */
export abstract class BaseLatencyHuntingStack extends cdk.Stack {
  protected readonly stateTable: dynamodb.Table;
  protected readonly instanceCreatorLambda: lambda.Function;
  protected readonly provider: Provider;

  constructor(scope: cdk.App, id: string, props: BaseLatencyHuntingStackProps) {
    super(scope, id, props);

    const maxInstancesPerType = props.maxInstancesPerType || 1;
    const managedByTag = props.managedByTag || 'CDK-LatencyHunting';

    // Create DynamoDB table for instance state tracking
    this.stateTable = new dynamodb.Table(this, 'InstanceStateTable', {
      partitionKey: { name: 'InstanceType', type: dynamodb.AttributeType.STRING },
      sortKey: { name: 'Timestamp', type: dynamodb.AttributeType.STRING },
      billingMode: dynamodb.BillingMode.PAY_PER_REQUEST,
      removalPolicy: RemovalPolicy.DESTROY,
      pointInTimeRecovery: false,
    });

    // Create Lambda function for resilient instance creation
    this.instanceCreatorLambda = new lambda.Function(this, 'InstanceCreatorLambda', {
      runtime: lambda.Runtime.PYTHON_3_11,
      handler: 'index.handler',
      timeout: Duration.minutes(5),
      environment: {
        STATE_TABLE_NAME: this.stateTable.tableName,
        MANAGED_BY_TAG: managedByTag,
      },
      code: lambda.Code.fromInline(`
import boto3
import json
import time
import random
import cfnresponse
import os
from datetime import datetime
from botocore.exceptions import ClientError

ec2 = boto3.client('ec2')
dynamodb = boto3.resource('dynamodb')
table = dynamodb.Table(os.environ['STATE_TABLE_NAME'])
managed_by_tag = os.environ['MANAGED_BY_TAG']

def write_state(instance_type, status, instance_id='', error='', architecture='', stack_id=''):
    """Write instance state to DynamoDB"""
    try:
        item = {
            'InstanceType': instance_type,
            'Timestamp': datetime.utcnow().isoformat(),
            'Status': status,
            'InstanceId': instance_id,
            'StackId': stack_id,
        }
        if error:
            item['Error'] = error
        if architecture:
            item['Architecture'] = architecture
        
        table.put_item(Item=item)
        print(f"Wrote state to DynamoDB: {instance_type} - {status}")
    except Exception as e:
        print(f"Error writing to DynamoDB: {str(e)}")
        # Don't fail the main operation if DynamoDB write fails

def handler(event, context):
    print(f"Event: {json.dumps(event)}")
    
    request_type = event['RequestType']
    
    if request_type == 'Delete':
        physical_resource_id = event.get('PhysicalResourceId')
        logical_resource_id = event.get('LogicalResourceId', '')
        stack_id = event.get('StackId', '')
        props = event.get('ResourceProperties', {})
        instance_type = props.get('InstanceType', 'unknown')
        
        instances_to_terminate = []
        
        # Always search by CloudFormationLogicalId tag - this is the reliable method
        print(f"Searching for instances with CloudFormationLogicalId: {logical_resource_id}")
        
        try:
            # Search for instances with matching CloudFormationLogicalId tag
            filters = [
                {'Name': 'tag:CloudFormationLogicalId', 'Values': [logical_resource_id]},
                {'Name': 'tag:ManagedBy', 'Values': [managed_by_tag]},
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
        else:
            # Write termination to state table
            for instance_id in instances_to_terminate:
                write_state(instance_type, 'terminated', instance_id, '', '', stack_id)
            
        cfnresponse.send(event, context, cfnresponse.SUCCESS, {})
        return
    
    if request_type == 'Update':
        physical_resource_id = event.get('PhysicalResourceId', '')
        old_props = event.get('OldResourceProperties', {})
        new_props = event.get('ResourceProperties', {})
        instance_type = new_props.get('InstanceType', 'unknown')
        stack_id = event.get('StackId', '')
        
        # Check if properties that require replacement have changed
        needs_replacement = (
            not physical_resource_id.startswith('i-') or  # Failed previous launch
            old_props.get('KeyName') != new_props.get('KeyName') or
            old_props.get('InstanceType') != new_props.get('InstanceType') or
            old_props.get('PlacementGroup') != new_props.get('PlacementGroup')
        )
        
        if needs_replacement:
            # Signal replacement by returning FAILED with a descriptive reason
            # CloudFormation will then orchestrate:
            # 1. Create a new resource (triggers Create handler with new instance)
            # 2. Delete old resource (triggers Delete handler to clean up old instance)
            print(f"Properties changed requiring replacement for {instance_type}")
            
            # Write state update to track replacement
            write_state(instance_type, 'replacement-needed', physical_resource_id, 
                       'Update triggered resource replacement', '', stack_id)
            
            # Return FAILURE to signal CloudFormation to replace the resource
            # This prevents duplicate instances by letting CloudFormation orchestrate
            cfnresponse.send(event, context, cfnresponse.FAILED, {
                'Error': 'Resource properties changed, replacement required',
                'InstanceType': instance_type,
                'Reason': 'KeyName, InstanceType, or PlacementGroup changed'
            }, physical_resource_id)
        else:
            # No replacement needed, instance stays as-is
            print(f"No replacement needed for {instance_type}, instance {physical_resource_id} unchanged")
            
            # Write state update
            write_state(instance_type, 'no-change', physical_resource_id, '', '', stack_id)
            
            cfnresponse.send(event, context, cfnresponse.SUCCESS, {
                'InstanceId': physical_resource_id,
                'InstanceType': instance_type,
                'Message': 'No changes required'
            }, physical_resource_id)
        
        # CRITICAL: Return here to prevent falling through to Create logic
        return
    
    # Create request
    props = event['ResourceProperties']
    instance_type = props.get('InstanceType', 'unknown')
    
    try:
        subnet_id = props['SubnetId']
        security_group_id = props['SecurityGroupId']
        key_name = props['KeyName']
        placement_group = props.get('PlacementGroup', '')
        
        delay = random.uniform(0, 5)
        print(f"Adding {delay:.2f}s delay to avoid API rate limiting")
        time.sleep(delay)
        
        # Detect architecture
        instance_types_info = ec2.describe_instance_types(InstanceTypes=[instance_type])
        architecture = instance_types_info['InstanceTypes'][0]['ProcessorInfo']['SupportedArchitectures'][0]
        print(f"Instance type {instance_type} architecture: {architecture}")
        
        # Get AMI
        ami_filters = [
            {'Name': 'name', 'Values': ['al2023-ami-*']},
            {'Name': 'state', 'Values': ['available']},
            {'Name': 'architecture', 'Values': [architecture]},
            {'Name': 'owner-alias', 'Values': ['amazon']}
        ]
        images = ec2.describe_images(Filters=ami_filters, Owners=['amazon'])
        if not images['Images']:
            raise Exception(f"No AMI found for architecture {architecture}")
        sorted_images = sorted(images['Images'], key=lambda x: x['CreationDate'], reverse=True)
        ami_id = sorted_images[0]['ImageId']
        
        user_data_script = '''#!/bin/bash
set -e
exec > >(tee /var/log/user-data.log)
exec 2>&1
echo "Starting EC2 Hunting setup at $(date)"
dnf install -y java-17-amazon-corretto wget unzip
mkdir -p /home/ec2-user/benchmark
cd /home/ec2-user/benchmark
wget -O ExchangeFlow-v1.0.0.jar https://github.com/aws-samples/trading-latency-benchmark/releases/download/v1.0.0/ExchangeFlow-v1.0.0.jar
wget -O config-samples-v1.0.0.zip https://github.com/aws-samples/trading-latency-benchmark/releases/download/v1.0.0/config-samples-v1.0.0.zip
unzip -q config-samples-v1.0.0.zip
CONFIG_FILE=$(find . -name "*config.properties" | head -n 1)
if [ -n "$CONFIG_FILE" ]; then
    cp "$CONFIG_FILE" config.properties
    sed -i 's/^WARMUP_COUNT=.*/WARMUP_COUNT=1/' config.properties
    sed -i 's/^TEST_SIZE=.*/TEST_SIZE=100/' config.properties
fi
chown -R ec2-user:ec2-user /home/ec2-user/benchmark
touch /home/ec2-user/benchmark/setup_complete
echo "EC2 Hunting setup completed at $(date)"
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
                    {'Key': 'ManagedBy', 'Value': managed_by_tag},
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
        
        # Retry logic
        max_retries = 5
        for attempt in range(max_retries):
            try:
                response = ec2.run_instances(**run_params)
                instance_id = response['Instances'][0]['InstanceId']
                print(f"Instance launched successfully on attempt {attempt + 1}")
                break
            except ClientError as e:
                if 'RequestLimitExceeded' in str(e) and attempt < max_retries - 1:
                    backoff_time = (2 ** attempt) + random.uniform(0, 1)
                    print(f"Rate limited, retrying in {backoff_time:.2f}s")
                    time.sleep(backoff_time)
                else:
                    raise
        
        # Wait for public IP
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
        
        # Write success to state table
        write_state(instance_type, 'launched', instance_id, '', architecture, event.get('StackId', ''))
        
        cfnresponse.send(event, context, cfnresponse.SUCCESS, {
            'InstanceId': instance_id,
            'InstanceType': instance_type,
            'Success': 'true',
            'PublicIp': public_ip or 'pending'
        }, instance_id)
        
    except Exception as e:
        error_msg = str(e)
        print(f"Failed to launch instance type {instance_type}: {error_msg}")
        
        graceful_error_patterns = [
            'InsufficientInstanceCapacity', 'Unsupported', 'InvalidInstanceType',
            'does not exist', 'is not supported', 'InstanceLimitExceeded',
            'VcpuLimitExceeded', 'RequestLimitExceeded', 'InvalidParameterCombination',
            'not supported for instances', 'not available'
        ]
        
        is_graceful_failure = any(pattern in error_msg for pattern in graceful_error_patterns)
        
        # Write failure to state table
        write_state(instance_type, 'failed', '', error_msg, '', event.get('StackId', ''))
        
        if is_graceful_failure:
            cfnresponse.send(event, context, cfnresponse.SUCCESS, {
                'InstanceId': 'failed',
                'InstanceType': instance_type,
                'Success': 'false',
                'Error': error_msg
            }, f'failed-{instance_type}')
        else:
            cfnresponse.send(event, context, cfnresponse.FAILED, {
                'Error': error_msg,
                'InstanceId': 'failed',
                'InstanceType': instance_type,
                'Success': 'false'
            })
`),
    });

    // Grant Lambda permissions for EC2
    this.instanceCreatorLambda.addToRolePolicy(new iam.PolicyStatement({
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

    // Grant Lambda permissions for DynamoDB
    this.stateTable.grantWriteData(this.instanceCreatorLambda);

    // Create custom resource provider
    this.provider = new Provider(this, 'InstanceCreatorProvider', {
      onEventHandler: this.instanceCreatorLambda,
    });
  }

  /**
   * Create instances for the provided instance types
   * @param instanceTypes Array of EC2 instance type strings
   * @param vpc VPC to deploy instances in
   * @param securityGroup Security group for instances
   * @param keyPair Key pair for SSH access
   * @param subnetId Subnet ID to deploy instances in
   */
  protected createInstances(
    instanceTypes: string[],
    vpc: IVpc,
    securityGroup: ISecurityGroup,
    keyPair: IKeyPair,
    subnetId: string
  ): void {
    instanceTypes.forEach((instanceType) => {
      const placementGroup = new CfnPlacementGroup(this, `PlacementGroup-${instanceType}`, {
        strategy: 'cluster'
      });
      placementGroup.applyRemovalPolicy(RemovalPolicy.DESTROY);

      const instanceResource = new CustomResource(this, `Instance-${instanceType}`, {
        serviceToken: this.provider.serviceToken,
        properties: {
          InstanceType: instanceType,
          SubnetId: subnetId,
          SecurityGroupId: securityGroup.securityGroupId,
          KeyName: keyPair.keyPairName,
          PlacementGroup: placementGroup.ref
        }
      });

      instanceResource.node.addDependency(placementGroup);
    });
  }

  /**
   * Add common stack outputs
   * @param instanceTypes Array of instance types being deployed
   * @param vpcId VPC ID
   * @param subnetId Subnet ID
   * @param securityGroupId Security group ID
   */
  protected addCommonOutputs(
    instanceTypes: string[],
    vpcId: string,
    subnetId: string,
    securityGroupId: string
  ): void {
    new cdk.CfnOutput(this, 'InstanceStateTableName', {
      value: this.stateTable.tableName,
      description: 'DynamoDB table name for instance state tracking'
    });

    new cdk.CfnOutput(this, 'TotalInstanceTypes', {
      value: instanceTypes.length.toString(),
      description: 'Total number of instance types attempted'
    });

    new cdk.CfnOutput(this, 'VpcId', {
      value: vpcId,
      description: 'VPC ID used for hunting instances'
    });

    new cdk.CfnOutput(this, 'SubnetId', {
      value: subnetId,
      description: 'Subnet ID used for hunting instances'
    });

    new cdk.CfnOutput(this, 'SecurityGroupId', {
      value: securityGroupId,
      description: 'Security group ID used'
    });

    new cdk.CfnOutput(this, 'Region', {
      value: this.region,
      description: 'Deployment region'
    });
  }

  /**
   * Get the default set of instance types for latency hunting
   * @returns Array of instance type strings
   */
  protected getDefaultInstanceTypes(): string[] {
    return [
      // Current generation - Intel
      'c8i.4xlarge', 'c7i.4xlarge', 'c6i.4xlarge', 'c5.4xlarge', 'c4.4xlarge', 'c3.4xlarge', 'c1.4xlarge',
      
      // Current generation - AMD
      'c8a.4xlarge', 'c7a.4xlarge', 'c6a.4xlarge', 'c5a.4xlarge', 
      
      // Current generation - Graviton ARM
      'c8g.4xlarge', 'c7g.4xlarge', 'c6g.4xlarge', 
    ];
  }
}
