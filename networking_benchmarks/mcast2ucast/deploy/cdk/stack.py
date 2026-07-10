from dataclasses import dataclass
from typing import Optional

from aws_cdk import (
    CfnOutput,
    Stack,
    Tags,
    aws_ec2 as ec2,
    aws_iam as iam,
)
from constructs import Construct


@dataclass(frozen=True)
class StackConfig:
    topology: str  # "cpg" | "same-az" | "multi-az" | "spread-az"
    sender_instance_type: str
    receiver_instance_type: str
    ami_id: str
    key_pair_name: str
    region: str
    single_az: Optional[str] = None
    sender_az: Optional[str] = None
    receiver_az: Optional[str] = None
    receiver_azs: Optional[list] = None  # spread-az only: ordered list of receiver AZs
    num_subscriber_hosts: int = 1
    num_subscriber_daemons_per_host: int = 1

    def __post_init__(self) -> None:
        # spread-az round-robins subscribers across receiver_azs; an empty
        # list would divide by zero at synth. Fail with a clear message
        # regardless of how the config was constructed (app.py or a test).
        if self.topology == "spread-az" and not self.receiver_azs:
            raise ValueError("spread-az topology requires a non-empty receiver_azs")

    def sender_az_resolved(self) -> str:
        if self.topology in ("multi-az", "spread-az"):
            if self.sender_az is None:
                raise ValueError(f"sender_az must be set for {self.topology} topology")
            return self.sender_az
        if self.single_az is None:
            raise ValueError("single_az must be set for non-multi-az topology")
        return self.single_az

    def receiver_az_resolved(self) -> str:
        if self.topology == "multi-az":
            if self.receiver_az is None:
                raise ValueError("receiver_az must be set for multi-az topology")
            return self.receiver_az
        if self.single_az is None:
            raise ValueError("single_az must be set for non-multi-az topology")
        return self.single_az


class Mcast2UcastBenchStack(Stack):
    def __init__(self, scope: Construct, construct_id: str, *, config: StackConfig, **kwargs) -> None:
        super().__init__(scope, construct_id, **kwargs)
        self.config = config

        Tags.of(self).add("Project", "mcast2ucast-bench")
        Tags.of(self).add("Topology", config.topology)

        self._vpc, self._sender_subnet, self._receiver_subnet, self._receiver_az_subnets = (
            self._build_vpc_and_subnets()
        )
        self._security_group = self._build_security_group()
        # None when topology in {same-az, multi-az, spread-az}; _build_host
        # checks before passing placement_group_name on CfnInstance.
        self._placement_group = self._build_placement_group()
        self._instance_profile = self._build_instance_profile()

        sender_inst, sender_eni = self._build_host(
            "sender", self.config.sender_instance_type, self._sender_subnet
        )
        self._emit_host_outputs(
            "sender", sender_inst, sender_eni, self.config.sender_az_resolved()
        )

        for i in range(self.config.num_subscriber_hosts):
            if self.config.topology == "spread-az":
                # Round-robin across the ordered receiver AZ list.
                receiver_azs = self.config.receiver_azs or []
                az = receiver_azs[i % len(receiver_azs)]
                sub_subnet = self._receiver_az_subnets[az]
            else:
                sub_subnet = self._receiver_subnet
                az = self.config.receiver_az_resolved()
            sub_inst, sub_eni = self._build_host(
                f"subscriber{i}",
                self.config.receiver_instance_type,
                sub_subnet,
            )
            self._emit_host_outputs(f"subscriber{i}", sub_inst, sub_eni, az)

        CfnOutput(self, "NumSubscriberHosts", value=str(self.config.num_subscriber_hosts))
        CfnOutput(self, "NumSubscriberDaemonsPerHost", value=str(self.config.num_subscriber_daemons_per_host))

        CfnOutput(self, "Topology", value=self.config.topology)
        CfnOutput(self, "KeyPairName", value=self.config.key_pair_name)
        CfnOutput(self, "AmiId", value=self.config.ami_id)

    # ------------------------------------------------------------------
    # VPC + subnets
    # ------------------------------------------------------------------
    def _build_vpc_and_subnets(
        self,
    ) -> tuple[ec2.CfnVPC, ec2.CfnSubnet, ec2.CfnSubnet, dict[str, ec2.CfnSubnet]]:
        """Return (vpc, sender_subnet, receiver_subnet, receiver_az_subnets).

        ``receiver_az_subnets`` is a dict mapping AZ name → CfnSubnet.  It is
        only populated for the ``spread-az`` topology; for all other topologies
        it is an empty dict and ``receiver_subnet`` carries the single receiver
        subnet as before.
        """
        vpc = ec2.CfnVPC(
            self,
            "Vpc",
            cidr_block="10.200.0.0/16",
            enable_dns_hostnames=True,
            enable_dns_support=True,
            tags=[{"key": "Name", "value": "mcast2ucast-bench-vpc"}],
        )
        igw = ec2.CfnInternetGateway(self, "Igw")
        ec2.CfnVPCGatewayAttachment(self, "IgwAttach", vpc_id=vpc.ref, internet_gateway_id=igw.ref)

        rt = ec2.CfnRouteTable(self, "PublicRt", vpc_id=vpc.ref)
        ec2.CfnRoute(
            self,
            "PublicDefaultRoute",
            route_table_id=rt.ref,
            destination_cidr_block="0.0.0.0/0",
            gateway_id=igw.ref,
        )

        sender_az = self.config.sender_az_resolved()

        sender_subnet = ec2.CfnSubnet(
            self,
            "SenderSubnet",
            vpc_id=vpc.ref,
            cidr_block="10.200.0.0/20",
            availability_zone=sender_az,
            map_public_ip_on_launch=True,
            tags=[{"key": "Name", "value": "mcast2ucast-bench-sender-subnet"}],
        )
        ec2.CfnSubnetRouteTableAssociation(
            self, "SenderSubnetRtAssoc", subnet_id=sender_subnet.ref, route_table_id=rt.ref
        )

        if self.config.topology == "spread-az":
            receiver_az_subnets = self._build_spread_az_subnets(vpc, rt)
            # receiver_subnet is unused for spread-az (callers use the dict),
            # but we return sender_subnet as a harmless placeholder to keep the
            # tuple shape consistent.
            receiver_subnet = sender_subnet
        elif self.config.topology == "multi-az":
            receiver_az = self.config.receiver_az_resolved()
            receiver_subnet = ec2.CfnSubnet(
                self,
                "ReceiverSubnet",
                vpc_id=vpc.ref,
                cidr_block="10.200.16.0/20",
                availability_zone=receiver_az,
                map_public_ip_on_launch=True,
                tags=[{"key": "Name", "value": "mcast2ucast-bench-receiver-subnet"}],
            )
            ec2.CfnSubnetRouteTableAssociation(
                self,
                "ReceiverSubnetRtAssoc",
                subnet_id=receiver_subnet.ref,
                route_table_id=rt.ref,
            )
            receiver_az_subnets = {}
        else:
            receiver_subnet = sender_subnet
            receiver_az_subnets = {}

        return vpc, sender_subnet, receiver_subnet, receiver_az_subnets

    def _build_spread_az_subnets(
        self,
        vpc: ec2.CfnVPC,
        rt: ec2.CfnRouteTable,
    ) -> dict[str, ec2.CfnSubnet]:
        """Create one subnet per unique AZ in ``receiver_azs``, using sequential
        /20 CIDRs starting at 10.200.16.0/20 (10.200.0.0/20 is the sender).

        Returns a dict mapping AZ name → CfnSubnet.
        """
        receiver_azs: list[str] = self.config.receiver_azs or []
        # Deduplicate while preserving order so CIDR assignment is stable.
        # dict.fromkeys keeps first-seen order in CPython 3.7+.
        unique_azs: list[str] = list(dict.fromkeys(receiver_azs))

        # Sender subnet occupies 10.200.0.0/20 (third octet 0).
        # Receiver subnets start at 10.200.16.0/20, stepping by 16 per AZ:
        #   AZ index 0 → 10.200.16.0/20
        #   AZ index 1 → 10.200.32.0/20
        #   AZ index 2 → 10.200.48.0/20
        #   AZ index 3 → 10.200.64.0/20
        az_subnets: dict[str, ec2.CfnSubnet] = {}
        for idx, az in enumerate(unique_azs):
            third_octet = 16 + idx * 16
            cidr = f"10.200.{third_octet}.0/20"
            # Use a safe logical ID suffix: replace hyphens and dots in the AZ
            # name (e.g. "us-east-2a" → "UsEast2A").
            az_suffix = "".join(part.capitalize() for part in az.replace("-", " ").replace(".", " ").split())
            subnet = ec2.CfnSubnet(
                self,
                f"ReceiverSubnet{az_suffix}",
                vpc_id=vpc.ref,
                cidr_block=cidr,
                availability_zone=az,
                map_public_ip_on_launch=True,
                tags=[{"key": "Name", "value": f"mcast2ucast-bench-receiver-subnet-{az}"}],
            )
            ec2.CfnSubnetRouteTableAssociation(
                self,
                f"ReceiverSubnet{az_suffix}RtAssoc",
                subnet_id=subnet.ref,
                route_table_id=rt.ref,
            )
            az_subnets[az] = subnet

        return az_subnets

    # ------------------------------------------------------------------
    # Cluster placement group (only when topology=cpg)
    # ------------------------------------------------------------------
    def _build_placement_group(self) -> Optional[ec2.CfnPlacementGroup]:
        if self.config.topology != "cpg":
            return None
        return ec2.CfnPlacementGroup(self, "ClusterPg", strategy="cluster")

    # ------------------------------------------------------------------
    # IAM instance profile (SSM fallback access)
    # ------------------------------------------------------------------
    def _build_instance_profile(self) -> iam.CfnInstanceProfile:
        role = iam.CfnRole(
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
            managed_policy_arns=[
                "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore"
            ],
            # Tags.of(self) does not propagate to L1 IAM resources.
            tags=[
                {"key": "Project", "value": "mcast2ucast-bench"},
                {"key": "Topology", "value": self.config.topology},
            ],
        )
        return iam.CfnInstanceProfile(
            self, "InstanceProfile", roles=[role.ref]
        )

    # ------------------------------------------------------------------
    # Per-role instance + secondary ENI
    # ------------------------------------------------------------------
    def _build_host(
        self,
        role: str,  # e.g. "sender" | "subscriber0" | "subscriber1"
        instance_type: str,
        subnet: ec2.CfnSubnet,
    ) -> tuple[ec2.CfnInstance, ec2.CfnNetworkInterface]:
        # Secondary ENI in the same subnet, source/dest check off
        secondary = ec2.CfnNetworkInterface(
            self,
            f"{role.capitalize()}SecondaryEni",
            subnet_id=subnet.ref,
            group_set=[self._security_group.ref],
            source_dest_check=False,
            description=f"mcast2ucast-bench {role} secondary ENI (DPDK)",
            tags=[
                {"key": "Name", "value": f"mcast2ucast-{role}-secondary-eni"},
                {"key": "Role", "value": role},
            ],
        )

        name_tag = f"mcast2ucast-{role}-{self.config.topology}"
        placement_kwargs: dict[str, str] = {}
        if self._placement_group is not None:
            placement_kwargs["placement_group_name"] = self._placement_group.ref

        instance = ec2.CfnInstance(
            self,
            f"{role.capitalize()}Instance",
            image_id=self.config.ami_id,
            instance_type=instance_type,
            subnet_id=subnet.ref,
            security_group_ids=[self._security_group.ref],
            key_name=self.config.key_pair_name,
            iam_instance_profile=self._instance_profile.ref,
            block_device_mappings=[
                ec2.CfnInstance.BlockDeviceMappingProperty(
                    device_name="/dev/xvda",
                    ebs=ec2.CfnInstance.EbsProperty(
                        volume_size=100,
                        volume_type="gp3",
                        delete_on_termination=True,
                    ),
                )
            ],
            tags=[
                {"key": "Name", "value": name_tag},
                {"key": "Role", "value": role},
                {"key": "Project", "value": "mcast2ucast-bench"},
            ],
            **placement_kwargs,
        )

        ec2.CfnNetworkInterfaceAttachment(
            self,
            f"{role.capitalize()}SecondaryAttach",
            instance_id=instance.ref,
            network_interface_id=secondary.ref,
            device_index="1",  # CFN schema: string, not int
            delete_on_termination=True,
        )

        return instance, secondary

    def _emit_host_outputs(
        self,
        role: str,
        instance: ec2.CfnInstance,
        secondary_eni: ec2.CfnNetworkInterface,
        az: str,
    ) -> None:
        prefix = role.capitalize()
        CfnOutput(self, f"{prefix}InstanceId", value=instance.ref)
        CfnOutput(self, f"{prefix}PrimaryIp", value=instance.attr_private_ip)
        CfnOutput(self, f"{prefix}PublicIp", value=instance.attr_public_ip)
        # Per-host AZ so the orchestrator records the true AZ of each host —
        # for spread-az each subscriber lands in a different AZ.
        CfnOutput(self, f"{prefix}Az", value=az)
        CfnOutput(self, f"{prefix}SecondaryEniId", value=secondary_eni.ref)
        CfnOutput(
            self,
            f"{prefix}SecondaryIp",
            value=secondary_eni.attr_primary_private_ip_address,
        )
        # MAC is not directly exposed as a CFN attribute on
        # AWS::EC2::NetworkInterface — orchestrator resolves via
        # describe-network-interfaces. We still emit the ENI ID for that.

    # ------------------------------------------------------------------
    # Security group
    # ------------------------------------------------------------------
    def _build_security_group(self) -> ec2.CfnSecurityGroup:
        sg = ec2.CfnSecurityGroup(
            self,
            "BenchSg",
            vpc_id=self._vpc.ref,
            group_description="mcast2ucast benchmark SG",
            security_group_egress=[
                ec2.CfnSecurityGroup.EgressProperty(
                    ip_protocol="-1", cidr_ip="0.0.0.0/0"
                )
            ],
            security_group_ingress=[
                ec2.CfnSecurityGroup.IngressProperty(
                    ip_protocol="tcp",
                    from_port=22,
                    to_port=22,
                    cidr_ip="0.0.0.0/0",
                    description="SSH from operator",
                ),
            ],
        )
        # Self-referencing ingress (UDP all + ICMP) — must be a separate
        # CfnSecurityGroupIngress because it references the SG itself.
        ec2.CfnSecurityGroupIngress(
            self,
            "BenchSgSelfUdp",
            group_id=sg.ref,
            ip_protocol="udp",
            from_port=0,
            to_port=65535,
            source_security_group_id=sg.ref,
            description="All UDP from peer",
        )
        ec2.CfnSecurityGroupIngress(
            self,
            "BenchSgSelfIcmp",
            group_id=sg.ref,
            ip_protocol="icmp",
            from_port=-1,
            to_port=-1,
            source_security_group_id=sg.ref,
            description="ICMP from peer",
        )
        CfnOutput(self, "VpcId", value=self._vpc.ref)
        CfnOutput(self, "SecurityGroupId", value=sg.ref)
        return sg
