import aws_cdk as cdk
from aws_cdk import aws_ec2 as ec2, aws_iam as iam, aws_logs as logs, aws_s3 as s3, Fn
from cdk_nag import NagSuppressions
from constructs import Construct


# Instance families that expose the ENA PTP Hardware Clock and support
# hardware RX packet timestamping via SO_TIMESTAMPING + SIOCSHWTSTAMP.
# See: https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/configure-ec2-ntp.html
# This benchmark requires HW RX timestamps on subscribers, so we gate
# instance_type to these families at synthesis time.
PHC_FAMILY_PREFIXES: tuple[str, ...] = (
    "m7", "c7", "r7", "i7", "i8", "c8", "m8", "r8", "x8",
)


def _validate_phc_instance(instance_type: str) -> None:
    """Raise ValueError if instance_type is not in a PHC-capable family.

    Accepts any 7th/8th-gen Nitro family (m7*, c7*, r7*, i7*, i8*, c8*, m8*,
    r8*, x8*). Rejects 6th-gen and earlier (e.g. c6in, m5, c5n).
    """
    family = instance_type.split(".", 1)[0].lower()
    if not any(family.startswith(prefix) for prefix in PHC_FAMILY_PREFIXES):
        raise ValueError(
            f"instance_type='{instance_type}' is not PHC-capable. "
            "The benchmark receiver requires hardware RX timestamping via the "
            "ENA PTP Hardware Clock, which is only available on 7th/8th-gen "
            "Nitro instances. Use e.g. m7i.large, c7i.large, r7i.large. "
            f"Allowed family prefixes: {', '.join(PHC_FAMILY_PREFIXES)}."
        )


class BenchmarkStack(cdk.Stack):
    def __init__(
        self,
        scope: Construct,
        construct_id: str,
        *,
        num_subscribers: int = 3,
        instance_type: str = "m7i.large",
        multicast_group: str = "239.1.1.1",
        multicast_port: int = 5001,
        s3_bucket_name: str | None = None,
        placement_strategy: str = "single-az-cpg",
        base_ami: str | None = None,
        **kwargs,
    ) -> None:
        super().__init__(scope, construct_id, **kwargs)

        # PHC gating: receiver-side HW timestamping requires a PHC-capable family.
        _validate_phc_instance(instance_type)

        # Store parameters for use by subsequent tasks
        self._num_subscribers = num_subscribers
        self._instance_type = instance_type
        self._multicast_group = multicast_group
        self._multicast_port = multicast_port
        self._s3_bucket_name = s3_bucket_name
        self._placement_strategy = placement_strategy
        self._base_ami = base_ami

        # --- VPC ---
        self.vpc = ec2.CfnVPC(
            self,
            "Vpc",
            cidr_block="10.0.0.0/16",
            enable_dns_support=True,
            enable_dns_hostnames=True,
            tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-vpc")],
        )

        # --- VPC Flow Log (AwsSolutions-VPC7) ---
        flow_log_group = logs.CfnLogGroup(
            self,
            "VpcFlowLogGroup",
            retention_in_days=7,
        )
        flow_log_role = iam.CfnRole(
            self,
            "VpcFlowLogRole",
            assume_role_policy_document={
                "Version": "2012-10-17",
                "Statement": [
                    {
                        "Effect": "Allow",
                        "Principal": {"Service": "vpc-flow-logs.amazonaws.com"},
                        "Action": "sts:AssumeRole",
                    }
                ],
            },
            policies=[
                iam.CfnRole.PolicyProperty(
                    policy_name="FlowLogPolicy",
                    policy_document={
                        "Version": "2012-10-17",
                        "Statement": [
                            {
                                "Effect": "Allow",
                                "Action": [
                                    "logs:CreateLogGroup",
                                    "logs:CreateLogStream",
                                    "logs:PutLogEvents",
                                    "logs:DescribeLogGroups",
                                    "logs:DescribeLogStreams",
                                ],
                                "Resource": "*",
                            }
                        ],
                    },
                ),
            ],
        )
        ec2.CfnFlowLog(
            self,
            "VpcFlowLog",
            resource_id=self.vpc.ref,
            resource_type="VPC",
            traffic_type="ALL",
            deliver_logs_permission_arn=flow_log_role.attr_arn,
            log_destination_type="cloud-watch-logs",
            log_group_name=flow_log_group.ref,
        )

        # --- Internet Gateway ---
        self.igw = ec2.CfnInternetGateway(
            self,
            "Igw",
            tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-igw")],
        )
        ec2.CfnVPCGatewayAttachment(
            self,
            "IgwAttachment",
            vpc_id=self.vpc.ref,
            internet_gateway_id=self.igw.ref,
        )

        # --- Public Route Table ---
        self.route_table = ec2.CfnRouteTable(
            self,
            "PublicRouteTable",
            vpc_id=self.vpc.ref,
            tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-public-rt")],
        )
        ec2.CfnRoute(
            self,
            "DefaultRoute",
            route_table_id=self.route_table.ref,
            destination_cidr_block="0.0.0.0/0",
            gateway_id=self.igw.ref,
        )

        # --- Placement Strategy ---
        azs = cdk.Fn.get_azs(self.region)

        if placement_strategy in ("single-az-cpg", "single-az"):
            # Both single-AZ strategies: 1 subnet in the first AZ
            first_az = cdk.Fn.select(0, azs)
            subnet = ec2.CfnSubnet(
                self,
                "PublicSubnet0",
                vpc_id=self.vpc.ref,
                cidr_block="10.0.0.0/24",
                availability_zone=first_az,
                map_public_ip_on_launch=True,
                tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-public-0")],
            )
            ec2.CfnSubnetRouteTableAssociation(
                self,
                "SubnetRtAssoc0",
                subnet_id=subnet.ref,
                route_table_id=self.route_table.ref,
            )
            self.subnets: list[ec2.CfnSubnet] = [subnet]
        elif placement_strategy == "cross-az":
            # Cross-AZ: create subnets across available AZs
            num_azs = 3  # use up to 3 AZs
            self.subnets: list[ec2.CfnSubnet] = []
            for i in range(num_azs):
                az = cdk.Fn.select(i, azs)
                subnet = ec2.CfnSubnet(
                    self,
                    f"PublicSubnet{i}",
                    vpc_id=self.vpc.ref,
                    cidr_block=f"10.0.{i}.0/24",
                    availability_zone=az,
                    map_public_ip_on_launch=True,
                    tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-public-{i}")],
                )
                ec2.CfnSubnetRouteTableAssociation(
                    self,
                    f"SubnetRtAssoc{i}",
                    subnet_id=subnet.ref,
                    route_table_id=self.route_table.ref,
                )
                self.subnets.append(subnet)
        else:
            raise ValueError(
                f"Invalid placement_strategy '{placement_strategy}'. "
                "Must be one of: 'single-az-cpg', 'single-az', 'cross-az'"
            )

        # --- Placement Group (only for single-az-cpg) ---
        if placement_strategy == "single-az-cpg":
            self.placement_group = ec2.CfnPlacementGroup(
                self,
                "ClusterPlacementGroup",
                strategy="cluster",
            )
        else:
            self.placement_group = None

        # --- Security Group ---
        self.security_group = ec2.CfnSecurityGroup(
            self,
            "BenchmarkSg",
            group_description="Security group for TGW multicast benchmark instances",
            vpc_id=self.vpc.ref,
            security_group_ingress=[
                ec2.CfnSecurityGroup.IngressProperty(
                    ip_protocol="udp",
                    from_port=multicast_port,
                    to_port=multicast_port,
                    cidr_ip="0.0.0.0/0",
                    description=f"Allow inbound UDP on multicast port {multicast_port}",
                ),
                ec2.CfnSecurityGroup.IngressProperty(
                    ip_protocol="tcp",
                    from_port=22,
                    to_port=22,
                    cidr_ip="0.0.0.0/0",
                    description="Allow inbound SSH",
                ),
            ],
            security_group_egress=[
                ec2.CfnSecurityGroup.EgressProperty(
                    ip_protocol="-1",
                    cidr_ip="0.0.0.0/0",
                    description="Allow all outbound traffic",
                ),
            ],
            tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-sg")],
        )

        # --- Transit Gateway (multicast enabled) ---
        self.tgw = ec2.CfnTransitGateway(
            self,
            "TransitGateway",
            multicast_support="enable",
            tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-tgw")],
        )

        # --- TGW VPC Attachment ---
        self.tgw_attachment = ec2.CfnTransitGatewayVpcAttachment(
            self,
            "TgwVpcAttachment",
            transit_gateway_id=self.tgw.ref,
            vpc_id=self.vpc.ref,
            subnet_ids=[subnet.ref for subnet in self.subnets],
            tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-tgw-attach")],
        )

        # --- Multicast Domain (IGMP disabled, static sources) ---
        self.multicast_domain = ec2.CfnTransitGatewayMulticastDomain(
            self,
            "MulticastDomain",
            transit_gateway_id=self.tgw.ref,
            tags=[cdk.CfnTag(key="Name", value=f"{construct_id}-mcast-domain")],
        )
        # Override Options with PascalCase keys — CDK synthesizes camelCase
        # but CloudFormation requires PascalCase for this resource.
        self.multicast_domain.add_property_override("Options", {
            "Igmpv2Support": "disable",
            "StaticSourcesSupport": "enable",
        })

        # --- Multicast Domain Association (one per subnet) ---
        self.multicast_domain_associations = []
        for i, subnet in enumerate(self.subnets):
            assoc = ec2.CfnTransitGatewayMulticastDomainAssociation(
                self,
                f"MulticastDomainAssoc{i}",
                transit_gateway_multicast_domain_id=self.multicast_domain.ref,
                transit_gateway_attachment_id=self.tgw_attachment.ref,
                subnet_id=subnet.ref,
            )
            self.multicast_domain_associations.append(assoc)

        # --- Synthesis-time validation ---
        if num_subscribers > 256:
            raise ValueError(
                f"num_subscribers={num_subscribers} exceeds the TGW multicast "
                "group member limit of 256"
            )

        # --- AMI selection ---
        if base_ami:
            ami_id = base_ami
        else:
            ami_id = ec2.MachineImage.latest_amazon_linux2023().get_image(self).image_id

        # --- S3 bucket for results ---
        bucket_props: dict = {}
        if s3_bucket_name:
            bucket_props["bucket_name"] = s3_bucket_name
        self.results_bucket = s3.CfnBucket(
            self,
            "ResultsBucket",
            **bucket_props,
        )

        # --- S3 bucket policy: enforce SSL (AwsSolutions-S10) ---
        s3.CfnBucketPolicy(
            self,
            "ResultsBucketPolicy",
            bucket=self.results_bucket.ref,
            policy_document={
                "Version": "2012-10-17",
                "Statement": [
                    {
                        "Sid": "EnforceSSL",
                        "Effect": "Deny",
                        "Principal": "*",
                        "Action": "s3:*",
                        "Resource": [
                            Fn.join("", ["arn:aws:s3:::", self.results_bucket.ref]),
                            Fn.join("", ["arn:aws:s3:::", self.results_bucket.ref, "/*"]),
                        ],
                        "Condition": {
                            "Bool": {"aws:SecureTransport": "false"},
                        },
                    }
                ],
            },
        )

        # --- IAM role for EC2 instances ---
        self.instance_role = iam.CfnRole(
            self,
            "InstanceRole",
            assume_role_policy_document={
                "Version": "2012-10-17",
                "Statement": [
                    {
                        "Effect": "Allow",
                        "Principal": {"Service": "ec2.amazonaws.com"},
                        "Action": "sts:AssumeRole",
                    }
                ],
            },
            policies=[
                iam.CfnRole.PolicyProperty(
                    policy_name="BenchmarkPolicy",
                    policy_document={
                        "Version": "2012-10-17",
                        "Statement": [
                            {
                                "Effect": "Allow",
                                "Action": "s3:PutObject",
                                "Resource": Fn.join("", [
                                    "arn:aws:s3:::",
                                    self.results_bucket.ref,
                                    "/*",
                                ]),
                            },
                            {
                                "Effect": "Allow",
                                "Action": "cloudwatch:PutMetricData",
                                "Resource": "*",
                            },
                        ],
                    },
                ),
            ],
            managed_policy_arns=[
                "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore",
            ],
        )

        self.instance_profile = iam.CfnInstanceProfile(
            self,
            "InstanceProfile",
            roles=[self.instance_role.ref],
        )

        # --- User data script ---
        user_data_script = """#!/bin/bash
set -x
exec > /var/log/benchmark-setup.log 2>&1

# ============================================================
# Phase 0: Kernel network tuning for low-latency multicast
# ============================================================
sysctl -w net.core.rmem_max=26214400
sysctl -w net.core.rmem_default=1048576
sysctl -w net.core.wmem_max=26214400
sysctl -w net.core.wmem_default=1048576
sysctl -w net.ipv4.conf.all.rp_filter=0
sysctl -w net.ipv4.conf.default.rp_filter=0
sysctl -w net.core.busy_poll=50
sysctl -w net.core.busy_read=50

# ============================================================
# Phase 1: Install all packages and build tools (network required)
# ============================================================

# Install iperf3 and build dependencies for sockperf
yum install -y iperf3 gcc gcc-c++ make automake autoconf libtool git

# Build sockperf from source (not in AL2023 repos)
if ! command -v sockperf &>/dev/null; then
    cd /tmp
    git clone https://github.com/Mellanox/sockperf.git
    cd sockperf
    ./autogen.sh
    ./configure --prefix=/usr/local
    make -j$(nproc)
    make install
    cd /
fi

# Verify tools are installed before proceeding
which sockperf || { echo "FATAL: sockperf install failed"; exit 1; }
which iperf3  || { echo "FATAL: iperf3 install failed"; exit 1; }
echo "Tools installed: sockperf=$(which sockperf), iperf3=$(which iperf3)"

# ============================================================
# Phase 2: Configure PTP clock sync (may briefly drop network)
# ============================================================

# Ensure PTP kernel module is loaded
modprobe ptp 2>/dev/null || true

# Enable PHC in ENA driver (off by default)
echo "options ena phc_enable=1" > /etc/modprobe.d/ena-phc.conf

# Reload ENA with PHC enabled if PTP device not already present
PTP_ACTIVE=false
for f in /sys/class/ptp/*/clock_name; do
    [ -f "$f" ] && grep -q "ena-ptp" "$f" && PTP_ACTIVE=true && break
done

if [ "$PTP_ACTIVE" = false ]; then
    echo "Reloading ENA driver with phc_enable=1..."
    rmmod ena 2>/dev/null
    modprobe ena phc_enable=1
    sleep 5
fi

# Check for ENA PTP hardware clock after reload
PTP_DEVICE=""
for f in /sys/class/ptp/*/clock_name; do
    if [ -f "$f" ] && grep -q "ena-ptp" "$f"; then
        PTP_DEVICE="/dev/$(basename $(dirname $f))"
        break
    fi
done

if [ -n "$PTP_DEVICE" ]; then
    echo "PTP hardware clock found at $PTP_DEVICE"
    # Create /dev/ptp_ena symlink
    if [ ! -e /dev/ptp_ena ]; then
        echo 'SUBSYSTEM=="ptp", ATTR{clock_name}=="ena-ptp-*", SYMLINK += "ptp_ena"' \
            >> /etc/udev/rules.d/53-ec2-network-interfaces.rules
        udevadm control --reload-rules && udevadm trigger
        sleep 1
    fi
    # Configure chrony with direct PTP connection
    grep -q "refclock PHC /dev/ptp_ena" /etc/chrony.conf || \
        echo 'refclock PHC /dev/ptp_ena poll 0 delay 0.000010 prefer' >> /etc/chrony.conf
else
    echo "No PTP hardware clock, using NTP via Amazon Time Sync Service"
    grep -q '169.254.169.123' /etc/chrony.conf || \
        echo 'server 169.254.169.123 prefer iburst minpoll 4 maxpoll 4' >> /etc/chrony.conf
fi

systemctl restart chronyd
chronyc waitsync 30 || true
chronyc sources

# ============================================================
# Phase 2b: Verify NIC HW timestamping capability (informational)
# ============================================================
# Find the primary network interface (the default route NIC) and log its
# ethtool -T output so we can confirm 'hardware-receive' is advertised and
# that a PHC index is present. The receiver script (mcast_recv.py) issues
# SIOCSHWTSTAMP itself at runtime; this is purely diagnostic.
PRIMARY_IFACE=$(ip -o -4 route show to default 2>/dev/null | awk '{print $5; exit}')
if [ -n "$PRIMARY_IFACE" ]; then
    echo "Primary interface: $PRIMARY_IFACE"
    ethtool -T "$PRIMARY_IFACE" || true
else
    echo "WARNING: could not determine primary interface"
fi

# ============================================================
# Phase 3: Signal readiness
# ============================================================
touch /opt/benchmark-env-ready
echo "Benchmark environment ready"
"""
        user_data_b64 = Fn.base64(user_data_script)

        # --- Helper to create ENI + Instance pair ---
        def _create_instance(
            logical_id: str,
            subnet: ec2.CfnSubnet,
            role_tag: str,
        ) -> tuple[ec2.CfnNetworkInterface, ec2.CfnInstance]:
            eni = ec2.CfnNetworkInterface(
                self,
                f"{logical_id}Eni",
                subnet_id=subnet.ref,
                group_set=[self.security_group.ref],
                source_dest_check=False,
                tags=[cdk.CfnTag(key="Role", value=role_tag)],
            )
            instance_props: dict = {
                "instance_type": instance_type,
                "image_id": ami_id,
                "user_data": user_data_b64,
                "iam_instance_profile": self.instance_profile.ref,
                "network_interfaces": [
                    ec2.CfnInstance.NetworkInterfaceProperty(
                        device_index="0",
                        network_interface_id=eni.ref,
                    )
                ],
                "block_device_mappings": [
                    ec2.CfnInstance.BlockDeviceMappingProperty(
                        device_name="/dev/xvda",
                        ebs=ec2.CfnInstance.EbsProperty(
                            encrypted=True,
                            volume_type="gp3",
                        ),
                    )
                ],
                "tags": [
                    cdk.CfnTag(key="Name", value=f"{construct_id}-{role_tag}"),
                    cdk.CfnTag(key="Role", value=role_tag),
                ],
            }
            if self.placement_group is not None:
                instance_props["placement_group_name"] = self.placement_group.ref
            inst = ec2.CfnInstance(self, logical_id, **instance_props)
            return eni, inst

        # --- Publisher instance ---
        pub_eni, self.publisher_instance = _create_instance(
            "Publisher", self.subnets[0], "publisher"
        )

        # --- Subscriber instances ---
        self.subscriber_instances: list[ec2.CfnInstance] = []
        subscriber_enis: list[ec2.CfnNetworkInterface] = []
        for i in range(num_subscribers):
            if placement_strategy == "cross-az":
                subnet = self.subnets[i % len(self.subnets)]
            else:
                subnet = self.subnets[0]
            eni, inst = _create_instance(f"Subscriber{i}", subnet, "subscriber")
            self.subscriber_instances.append(inst)
            subscriber_enis.append(eni)

        # --- Multicast group registrations ---
        # Publisher as source
        for assoc in self.multicast_domain_associations:
            pass  # ensure associations exist before registrations

        pub_source = ec2.CfnTransitGatewayMulticastGroupSource(
            self,
            "PublisherMulticastSource",
            transit_gateway_multicast_domain_id=self.multicast_domain.ref,
            group_ip_address=multicast_group,
            network_interface_id=pub_eni.ref,
        )
        # Ensure domain associations complete before registrations
        for assoc in self.multicast_domain_associations:
            pub_source.add_dependency(assoc)

        # Subscribers as members
        for i, sub_eni in enumerate(subscriber_enis):
            member = ec2.CfnTransitGatewayMulticastGroupMember(
                self,
                f"SubscriberMulticastMember{i}",
                transit_gateway_multicast_domain_id=self.multicast_domain.ref,
                group_ip_address=multicast_group,
                network_interface_id=sub_eni.ref,
            )
            for assoc in self.multicast_domain_associations:
                member.add_dependency(assoc)

        # --- Stack outputs ---
        cdk.CfnOutput(
            self,
            "PublisherInstanceId",
            value=self.publisher_instance.ref,
            description="Instance ID of the publisher",
        )
        cdk.CfnOutput(
            self,
            "SubscriberInstanceIds",
            value=Fn.join(",", [inst.ref for inst in self.subscriber_instances]),
            description="Comma-separated instance IDs of subscribers",
        )
        cdk.CfnOutput(
            self,
            "S3BucketName",
            value=self.results_bucket.ref,
            description="S3 bucket for benchmark results",
        )
        cdk.CfnOutput(
            self,
            "MulticastGroup",
            value=multicast_group,
            description="Multicast group address",
        )
        cdk.CfnOutput(
            self,
            "MulticastPort",
            value=str(multicast_port),
            description="Multicast UDP port",
        )

        # --- cdk-nag suppressions for non-critical findings ---
        # These are acceptable for a short-lived benchmark environment.

        NagSuppressions.add_resource_suppressions(
            self.security_group,
            [
                {
                    "id": "AwsSolutions-EC23",
                    "reason": "Benchmark SG needs 0.0.0.0/0 inbound for UDP multicast from TGW and SSH access during testing.",
                },
            ],
        )

        NagSuppressions.add_resource_suppressions(
            self.results_bucket,
            [
                {
                    "id": "AwsSolutions-S1",
                    "reason": "Access logging not required for ephemeral benchmark results bucket.",
                },
            ],
        )

        NagSuppressions.add_resource_suppressions(
            self.instance_role,
            [
                {
                    "id": "AwsSolutions-IAM4",
                    "reason": "AmazonSSMManagedInstanceCore managed policy is required for SSM Run Command access.",
                    "appliesTo": [
                        "Policy::arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore",
                    ],
                },
                {
                    "id": "AwsSolutions-IAM5",
                    "reason": "S3 PutObject wildcard scoped to results bucket path. CloudWatch PutMetricData requires Resource: *.",
                    "appliesTo": [
                        "Resource::arn:aws:s3:::<ResultsBucket>/*",
                        "Resource::*",
                    ],
                },
            ],
        )

        NagSuppressions.add_resource_suppressions(
            flow_log_role,
            [
                {
                    "id": "AwsSolutions-IAM5",
                    "reason": "VPC Flow Log role needs logs:* on Resource: * to write to any log stream.",
                    "appliesTo": ["Resource::*"],
                },
            ],
        )

        all_instances = [self.publisher_instance] + self.subscriber_instances
        for inst in all_instances:
            NagSuppressions.add_resource_suppressions(
                inst,
                [
                    {
                        "id": "AwsSolutions-EC28",
                        "reason": "Detailed monitoring adds cost with no benefit for short-lived benchmark instances.",
                    },
                    {
                        "id": "AwsSolutions-EC29",
                        "reason": "Benchmark instances are ephemeral — termination protection and ASG are not needed.",
                    },
                ],
            )

