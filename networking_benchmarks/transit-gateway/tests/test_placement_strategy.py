"""Tests for placement strategy logic (Task 1.3).

Validates Requirements 1.2, 1.3, 1.4, 1.5.
"""

import aws_cdk as cdk
from aws_cdk.assertions import Template
import pytest

from stacks.benchmark_stack import BenchmarkStack


def _synth(placement_strategy: str) -> Template:
    app = cdk.App()
    stack = BenchmarkStack(
        app,
        "TestStack",
        placement_strategy=placement_strategy,
        env=cdk.Environment(account="123456789012", region="us-east-1"),
    )
    return Template.from_stack(stack)


class TestSingleAzCpg:
    """Req 1.3: single-az-cpg creates 1 subnet + cluster placement group."""

    def test_creates_one_subnet(self):
        template = _synth("single-az-cpg")
        template.resource_count_is("AWS::EC2::Subnet", 1)

    def test_creates_cluster_placement_group(self):
        template = _synth("single-az-cpg")
        template.resource_count_is("AWS::EC2::PlacementGroup", 1)
        template.has_resource_properties(
            "AWS::EC2::PlacementGroup",
            {"Strategy": "cluster"},
        )

    def test_stack_exposes_placement_group(self):
        app = cdk.App()
        stack = BenchmarkStack(
            app,
            "TestStack",
            placement_strategy="single-az-cpg",
            env=cdk.Environment(account="123456789012", region="us-east-1"),
        )
        assert stack.placement_group is not None

    def test_subnets_is_list_of_one(self):
        app = cdk.App()
        stack = BenchmarkStack(
            app,
            "TestStack",
            placement_strategy="single-az-cpg",
            env=cdk.Environment(account="123456789012", region="us-east-1"),
        )
        assert len(stack.subnets) == 1


class TestSingleAz:
    """Req 1.4: single-az creates 1 subnet, no placement group."""

    def test_creates_one_subnet(self):
        template = _synth("single-az")
        template.resource_count_is("AWS::EC2::Subnet", 1)

    def test_no_placement_group(self):
        template = _synth("single-az")
        template.resource_count_is("AWS::EC2::PlacementGroup", 0)

    def test_placement_group_is_none(self):
        app = cdk.App()
        stack = BenchmarkStack(
            app,
            "TestStack",
            placement_strategy="single-az",
            env=cdk.Environment(account="123456789012", region="us-east-1"),
        )
        assert stack.placement_group is None


class TestCrossAz:
    """Req 1.5: cross-az creates subnets across AZs."""

    def test_creates_multiple_subnets(self):
        template = _synth("cross-az")
        template.resource_count_is("AWS::EC2::Subnet", 3)

    def test_no_placement_group(self):
        template = _synth("cross-az")
        template.resource_count_is("AWS::EC2::PlacementGroup", 0)

    def test_placement_group_is_none(self):
        app = cdk.App()
        stack = BenchmarkStack(
            app,
            "TestStack",
            placement_strategy="cross-az",
            env=cdk.Environment(account="123456789012", region="us-east-1"),
        )
        assert stack.placement_group is None

    def test_subnets_list_has_three(self):
        app = cdk.App()
        stack = BenchmarkStack(
            app,
            "TestStack",
            placement_strategy="cross-az",
            env=cdk.Environment(account="123456789012", region="us-east-1"),
        )
        assert len(stack.subnets) == 3

    def test_each_subnet_has_route_table_association(self):
        template = _synth("cross-az")
        template.resource_count_is("AWS::EC2::SubnetRouteTableAssociation", 3)


class TestInvalidStrategy:
    """Req 1.2: invalid placement_strategy raises ValueError."""

    def test_invalid_strategy_raises(self):
        with pytest.raises(ValueError, match="Invalid placement_strategy"):
            _synth("invalid-strategy")
