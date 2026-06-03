"""Property test: Subscriber count matches configuration.

Feature: tgw-multicast-benchmark, Property 1: Subscriber count matches configuration

**Validates: Requirements 2.6, 3.2**

For any num_subscribers in [1, 10], the synthesized CDK template SHALL contain
exactly num_subscribers EC2 instance resources with the subscriber role AND
exactly num_subscribers CfnTransitGatewayMulticastGroupMember resources.
"""

import aws_cdk as cdk
from aws_cdk.assertions import Template
from hypothesis import given, settings
from hypothesis.strategies import integers

from stacks.benchmark_stack import BenchmarkStack


def _synth_template(num_subscribers: int) -> dict:
    """Synthesize a BenchmarkStack and return the raw CloudFormation JSON."""
    app = cdk.App()
    stack = BenchmarkStack(
        app,
        "PropTestStack",
        num_subscribers=num_subscribers,
        env=cdk.Environment(account="123456789012", region="us-east-1"),
    )
    template = Template.from_stack(stack)
    return template.to_json()


def _count_subscriber_instances(tpl: dict) -> int:
    """Count EC2 instances tagged with Role=subscriber."""
    count = 0
    for resource in tpl["Resources"].values():
        if resource["Type"] != "AWS::EC2::Instance":
            continue
        tags = resource.get("Properties", {}).get("Tags", [])
        for tag in tags:
            if tag.get("Key") == "Role" and tag.get("Value") == "subscriber":
                count += 1
                break
    return count


def _count_multicast_group_members(tpl: dict) -> int:
    """Count CfnTransitGatewayMulticastGroupMember resources."""
    return sum(
        1
        for r in tpl["Resources"].values()
        if r["Type"] == "AWS::EC2::TransitGatewayMulticastGroupMember"
    )


def _count_total_instances(tpl: dict) -> int:
    """Count all EC2 instance resources."""
    return sum(
        1
        for r in tpl["Resources"].values()
        if r["Type"] == "AWS::EC2::Instance"
    )


@given(num_subscribers=integers(min_value=1, max_value=10))
@settings(max_examples=100, deadline=None)
def test_subscriber_count_matches_configuration(num_subscribers: int) -> None:
    """Property 1: Subscriber count matches configuration.

    Feature: tgw-multicast-benchmark, Property 1: Subscriber count matches configuration
    **Validates: Requirements 2.6, 3.2**
    """
    tpl = _synth_template(num_subscribers)

    subscriber_count = _count_subscriber_instances(tpl)
    assert subscriber_count == num_subscribers, (
        f"Expected {num_subscribers} subscriber instances, got {subscriber_count}"
    )

    member_count = _count_multicast_group_members(tpl)
    assert member_count == num_subscribers, (
        f"Expected {num_subscribers} multicast group members, got {member_count}"
    )

    total_instances = _count_total_instances(tpl)
    assert total_instances == num_subscribers + 1, (
        f"Expected {num_subscribers + 1} total instances "
        f"(1 publisher + {num_subscribers} subscribers), got {total_instances}"
    )
