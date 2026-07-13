import aws_cdk as cdk
from aws_cdk.assertions import Match, Template

from stack import Mcast2UcastBenchStack, StackConfig


def _synth(
    topology: str = "cpg",
    num_subscriber_hosts: int = 1,
    num_subscriber_daemons_per_host: int = 1,
    **overrides,
) -> Template:
    config = StackConfig(
        topology=topology,
        sender_instance_type=overrides.get("sender_instance_type", "m8a.2xlarge"),
        receiver_instance_type=overrides.get("receiver_instance_type", "m8a.2xlarge"),
        ami_id="ami-test",
        key_pair_name="test-key",
        region="us-east-1",
        single_az=overrides.get("single_az", "us-east-1a") if topology != "multi-az" else None,
        sender_az=overrides.get("sender_az", "us-east-1a") if topology == "multi-az" else None,
        receiver_az=overrides.get("receiver_az", "us-east-1b") if topology == "multi-az" else None,
        num_subscriber_hosts=num_subscriber_hosts,
        num_subscriber_daemons_per_host=num_subscriber_daemons_per_host,
    )
    app = cdk.App()
    stack = Mcast2UcastBenchStack(
        app,
        "TestStack",
        config=config,
        # account is unset in unit tests — Template.from_stack works with
        # an env-less stack as long as no resource pulls account-specific
        # context (we only emit Cfn primitives, so this is fine).
        env=cdk.Environment(region="us-east-1"),
    )
    return Template.from_stack(stack)


def test_cpg_creates_placement_group():
    template = _synth(topology="cpg")
    template.resource_count_is("AWS::EC2::PlacementGroup", 1)
    template.has_resource_properties("AWS::EC2::PlacementGroup", {"Strategy": "cluster"})


def test_same_az_no_placement_group():
    template = _synth(topology="same-az")
    template.resource_count_is("AWS::EC2::PlacementGroup", 0)
    # Also assert no instance carries a stale PlacementGroupName property —
    # guards against a refactor that hardcodes the name even when the CPG
    # resource is correctly omitted.
    for inst in template.find_resources("AWS::EC2::Instance").values():
        assert "PlacementGroupName" not in inst["Properties"], (
            "same-az instances must not have PlacementGroupName"
        )


def test_multi_az_uses_two_subnets():
    template = _synth(topology="multi-az")
    instances = template.find_resources("AWS::EC2::Instance")
    subnet_refs = set()
    for inst in instances.values():
        sn = inst["Properties"].get("SubnetId")
        if isinstance(sn, dict) and "Ref" in sn:
            subnet_refs.add(sn["Ref"])
        elif isinstance(sn, str):
            subnet_refs.add(sn)
    assert len(subnet_refs) == 2, (
        f"multi-az should put sender and receiver in different subnets, "
        f"found refs: {subnet_refs}"
    )

    # Also assert the two subnets land in distinct AZs — guards against a
    # refactor that creates two CfnSubnet constructs in the same AZ.
    subnets = template.find_resources("AWS::EC2::Subnet")
    azs = {s["Properties"]["AvailabilityZone"] for s in subnets.values()}
    assert len(azs) == 2, f"multi-az should span 2 AZs, found: {azs}"


def test_secondary_eni_attached_at_index_one():
    template = _synth(topology="cpg", num_subscriber_hosts=1)
    attachments = template.find_resources("AWS::EC2::NetworkInterfaceAttachment")
    # 1 sender + 1 subscriber = 2
    assert len(attachments) == 2, f"expected 2 ENI attachments, got {len(attachments)}"
    for attach in attachments.values():
        props = attach["Properties"]
        assert props["DeviceIndex"] == "1"
        assert props["DeleteOnTermination"] is True


def test_security_group_allows_intra_udp():
    template = _synth(topology="cpg")
    template.has_resource_properties(
        "AWS::EC2::SecurityGroupIngress",
        {
            "IpProtocol": "udp",
            "FromPort": 0,
            "ToPort": 65535,
            "SourceSecurityGroupId": Match.any_value(),
        },
    )


def test_multiple_subscriber_hosts_create_separate_instances():
    template = _synth(topology="cpg", num_subscriber_hosts=4)
    instances = template.find_resources("AWS::EC2::Instance")
    # 1 sender + 4 subscribers = 5
    assert len(instances) == 5, f"expected 5 instances, got {len(instances)}"
    enis = template.find_resources("AWS::EC2::NetworkInterface")
    assert len(enis) == 5, f"expected 5 secondary ENIs, got {len(enis)}"
    attachments = template.find_resources("AWS::EC2::NetworkInterfaceAttachment")
    assert len(attachments) == 5, f"expected 5 ENI attachments, got {len(attachments)}"


def test_h_times_d_validation_in_stack_config():
    # Stack itself doesn't validate H*D <= 128 (app.py does).
    # Just confirm the StackConfig accepts the values without crashing.
    template = _synth(topology="cpg", num_subscriber_hosts=8)
    instances = template.find_resources("AWS::EC2::Instance")
    assert len(instances) == 9  # 1 sender + 8 subscribers


def test_subscriber_output_keys_not_receiver():
    # Lock down the CFN output contract Task 11b consumes. A future refactor
    # that resurrects the "receiver" role string would silently break it.
    template = _synth(topology="cpg", num_subscriber_hosts=2)
    outputs = template.to_json().get("Outputs", {})
    assert "Subscriber0InstanceId" in outputs, "expected Subscriber0InstanceId output"
    assert "Subscriber1InstanceId" in outputs, "expected Subscriber1InstanceId output"
    assert "ReceiverInstanceId" not in outputs, "ReceiverInstanceId must not appear (old naming)"
    assert "NumSubscriberHosts" in outputs
    assert "NumSubscriberDaemonsPerHost" in outputs
