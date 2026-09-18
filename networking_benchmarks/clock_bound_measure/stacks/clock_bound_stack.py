# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
import os
from aws_cdk import (
    Stack,
    CfnParameter,
    CfnOutput,
    Duration,
    RemovalPolicy,
    Tags,
    aws_cloudwatch as cw,
    aws_ec2 as ec2,
    aws_iam as iam,
    aws_s3_assets as s3_assets,
)
from constructs import Construct

METRIC_NAMESPACE = "ClockBoundMeasure"
DASHBOARD_NAME = "ClockBoundMeasure"


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

        # PTP and NTP baselines differ by ~20x; a shared threshold would sit
        # permanently red on one instance and never fire on the other.
        threshold_1 = CfnParameter(
            self,
            "ClockErrorBoundThreshold1",
            type="Number",
            default=200,
            description="ClockErrorBound alarm threshold (us) for the first instance",
        )

        threshold_2 = CfnParameter(
            self,
            "ClockErrorBoundThreshold2",
            type="Number",
            default=2000,
            description="ClockErrorBound alarm threshold (us) for the second instance",
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

        role.add_to_policy(
            iam.PolicyStatement(
                actions=["cloudwatch:PutMetricData"],
                resources=["*"],
                conditions={
                    "StringEquals": {"cloudwatch:namespace": METRIC_NAMESPACE}
                },
            )
        )

        ami = ec2.MachineImage.latest_amazon_linux2023()

        user_data_dir = os.path.join(os.path.dirname(__file__), "..", "user_data")
        with open(os.path.join(user_data_dir, "bootstrap.sh"), "r") as f:
            user_data_script = f.read()

        publisher = s3_assets.Asset(
            self,
            "MetricsPublisher",
            path=os.path.join(user_data_dir, "clockbound_metrics.py"),
        )
        publisher.grant_read(role)

        # user_data runs on first boot only, so both instances below set
        # user_data_causes_replacement: otherwise editing bootstrap.sh or the
        # publisher deploys as a silent no-op.
        user_data = ec2.UserData.for_linux()
        # Download first: bootstrap.sh installs a unit pointing at this path.
        user_data.add_s3_download_command(
            bucket=publisher.bucket,
            bucket_key=publisher.s3_object_key,
            local_file="/opt/clockbound_metrics.py",
        )
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
            user_data_causes_replacement=True,
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
            user_data_causes_replacement=True,
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
        Tags.of(instance2).add("Name", "ClockBound-2")
        Tags.of(instance2).add("Project", "clock-bound-measure")

        # ── CloudWatch monitoring ────────────────────────────────────────────
        # Unit left unset: a mismatch with the publisher yields INSUFFICIENT_DATA.
        def metric(name, instance_id):
            return cw.Metric(
                namespace=METRIC_NAMESPACE,
                metric_name=name,
                dimensions_map={"InstanceId": instance_id},
                statistic="Maximum",
                period=Duration.minutes(1),
            )

        alarms = []
        bound_metrics = []
        status_metrics = []
        annotations = []

        for idx, (instance, threshold) in enumerate(
            [(instance1, threshold_1), (instance2, threshold_2)], start=1
        ):
            bound = metric("ClockErrorBound", instance.instance_id)
            status = metric("ClockStatus", instance.instance_id)
            bound_metrics.append(bound)
            status_metrics.append(status)
            annotations.append(
                cw.HorizontalAnnotation(
                    value=threshold.value_as_number,
                    label=f"Instance {idx} threshold",
                )
            )
            # The publisher emits nothing when it cannot read a valid segment,
            # so a missing datapoint means the bound is unknown: a breach.
            alarms.append(
                cw.Alarm(
                    self,
                    f"ClockErrorBoundAlarm{idx}",
                    metric=bound,
                    threshold=threshold.value_as_number,
                    evaluation_periods=1,
                    comparison_operator=cw.ComparisonOperator.GREATER_THAN_THRESHOLD,
                    treat_missing_data=cw.TreatMissingData.BREACHING,
                    alarm_description=(
                        f"Instance {idx} ClockBound error bound exceeded threshold"
                    ),
                )
            )
            alarms.append(
                cw.Alarm(
                    self,
                    f"ClockStatusAlarm{idx}",
                    metric=status,
                    threshold=2,
                    evaluation_periods=1,
                    comparison_operator=(
                        cw.ComparisonOperator.GREATER_THAN_OR_EQUAL_TO_THRESHOLD
                    ),
                    treat_missing_data=cw.TreatMissingData.BREACHING,
                    alarm_description=(
                        f"Instance {idx} clock is FreeRunning or Disrupted"
                    ),
                )
            )

        dashboard = cw.Dashboard(
            self, "ClockBoundDashboard", dashboard_name=DASHBOARD_NAME
        )
        dashboard.add_widgets(
            cw.GraphWidget(
                title="ClockBound error bound (us)",
                left=bound_metrics,
                left_annotations=annotations,
                width=12,
            ),
            cw.GraphWidget(
                title="Clock status (1=Synchronized 2=FreeRunning 3=Disrupted)",
                left=status_metrics,
                width=12,
            ),
            cw.AlarmStatusWidget(alarms=alarms, width=24),
        )

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
        CfnOutput(
            self,
            "DashboardUrl",
            value=(
                f"https://{self.region}.console.aws.amazon.com/cloudwatch/home"
                f"?region={self.region}#dashboards:name={dashboard.dashboard_name}"
            ),
        )
