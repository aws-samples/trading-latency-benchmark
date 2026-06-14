# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
import os
from aws_cdk import (
    Stack,
    CfnParameter,
    CfnOutput,
    RemovalPolicy,
    Tags,
    aws_ec2 as ec2,
    aws_iam as iam,
)
from constructs import Construct


class ClockBoundMeasureStack(Stack):
    def __init__(self, scope: Construct, construct_id: str, **kwargs) -> None:
        super().__init__(scope, construct_id, **kwargs)

        instance_type_1 = CfnParameter(
            self,
            "InstanceType1",
            type="String",
            default="c6in.4xlarge",
            description="Instance type for the first EC2 instance",
        )

        instance_type_2 = CfnParameter(
            self,
            "InstanceType2",
            type="String",
            default="c6in.4xlarge",
            description="Instance type for the second EC2 instance",
        )

        vpc = ec2.Vpc(
            self,
            "ClockBoundVpc",
            ip_addresses=ec2.IpAddresses.cidr("10.60.0.0/16"),
            nat_gateways=1,
            max_azs=2,
            subnet_configuration=[
                ec2.SubnetConfiguration(
                    name="Public",
                    subnet_type=ec2.SubnetType.PUBLIC,
                    cidr_mask=24,
                ),
                ec2.SubnetConfiguration(
                    name="Private",
                    subnet_type=ec2.SubnetType.PRIVATE_WITH_EGRESS,
                    cidr_mask=24,
                ),
            ],
            gateway_endpoints={},
        )
        vpc.apply_removal_policy(RemovalPolicy.DESTROY)

        sg = ec2.SecurityGroup(
            self,
            "ClockBoundSG",
            vpc=vpc,
            description="ClockBound instances - egress only",
            allow_all_outbound=True,
        )
        sg.apply_removal_policy(RemovalPolicy.DESTROY)

        role = iam.Role(
            self,
            "ClockBoundInstanceRole",
            assumed_by=iam.ServicePrincipal("ec2.amazonaws.com"),
            managed_policies=[
                iam.ManagedPolicy.from_aws_managed_policy_name(
                    "AmazonSSMManagedInstanceCore"
                )
            ],
        )

        ami = ec2.MachineImage.latest_amazon_linux2023()

        user_data_path = os.path.join(
            os.path.dirname(__file__), "..", "user_data", "bootstrap.sh"
        )
        with open(user_data_path, "r") as f:
            user_data_script = f.read()

        user_data = ec2.UserData.for_linux()
        user_data.add_commands(user_data_script)

        private_subnets = ec2.SubnetSelection(
            subnet_type=ec2.SubnetType.PRIVATE_WITH_EGRESS
        )

        instance1 = ec2.Instance(
            self,
            "ClockBoundInstance1",
            instance_type=ec2.InstanceType(instance_type_1.value_as_string),
            machine_image=ami,
            vpc=vpc,
            vpc_subnets=private_subnets,
            security_group=sg,
            role=role,
            user_data=user_data,
            block_devices=[
                ec2.BlockDevice(
                    device_name="/dev/xvda",
                    volume=ec2.BlockDeviceVolume.ebs(
                        50, volume_type=ec2.EbsDeviceVolumeType.GP3
                    ),
                )
            ],
        )
        instance1.apply_removal_policy(RemovalPolicy.DESTROY)
        Tags.of(instance1).add("Name", "ClockBound-1")
        Tags.of(instance1).add("Project", "clock-bound-measure")

        instance2 = ec2.Instance(
            self,
            "ClockBoundInstance2",
            instance_type=ec2.InstanceType(instance_type_2.value_as_string),
            machine_image=ami,
            vpc=vpc,
            vpc_subnets=private_subnets,
            security_group=sg,
            role=role,
            user_data=user_data,
            block_devices=[
                ec2.BlockDevice(
                    device_name="/dev/xvda",
                    volume=ec2.BlockDeviceVolume.ebs(
                        50, volume_type=ec2.EbsDeviceVolumeType.GP3
                    ),
                )
            ],
        )
        instance2.apply_removal_policy(RemovalPolicy.DESTROY)
        Tags.of(instance2).add("Name", f"ClockBound-2")
        Tags.of(instance2).add("Project", "clock-bound-measure")

        CfnOutput(self, "Instance1Id", value=instance1.instance_id)
        CfnOutput(self, "Instance2Id", value=instance2.instance_id)
        CfnOutput(
            self,
            "Instance1Type",
            value=instance_type_1.value_as_string,
        )
        CfnOutput(
            self,
            "Instance2Type",
            value=instance_type_2.value_as_string,
        )
        CfnOutput(self, "StackRegion", value=self.region)
