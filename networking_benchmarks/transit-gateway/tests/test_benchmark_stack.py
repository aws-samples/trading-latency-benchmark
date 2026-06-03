"""CDK template assertion tests for BenchmarkStack (Task 1.6).

Validates Requirements: 2.1, 2.3, 1.6, 3.3, 3.4, 3.6, 5.2, 5.3.
"""

import json

import aws_cdk as cdk
from aws_cdk.assertions import Match, Template
import pytest

from stacks.benchmark_stack import BenchmarkStack


def _synth(**kwargs) -> Template:
    """Synthesize a BenchmarkStack with given overrides and return the Template."""
    app = cdk.App()
    defaults = {
        "env": cdk.Environment(account="123456789012", region="us-east-1"),
    }
    defaults.update(kwargs)
    stack = BenchmarkStack(app, "TestStack", **defaults)
    return Template.from_stack(stack)


def _get_user_data_script(template: Template) -> str:
    """Extract the user data script string from the first EC2 instance in the template."""
    tpl = template.to_json()
    for _logical_id, resource in tpl["Resources"].items():
        if resource["Type"] == "AWS::EC2::Instance":
            user_data = resource["Properties"].get("UserData")
            if user_data and isinstance(user_data, dict) and "Fn::Base64" in user_data:
                return user_data["Fn::Base64"]
    return ""


class TestTransitGateway:
    """Req 2.1: TGW has multicast enabled."""

    def test_tgw_multicast_enabled(self):
        template = _synth()
        template.has_resource_properties(
            "AWS::EC2::TransitGateway",
            {"MulticastSupport": "enable"},
        )


class TestMulticastDomain:
    """Req 2.3: Multicast domain has IGMP disabled and static sources enabled."""

    def test_igmp_disabled_static_sources_enabled(self):
        template = _synth()
        template.has_resource_properties(
            "AWS::EC2::TransitGatewayMulticastDomain",
            {
                "Options": {
                    "Igmpv2Support": "disable",
                    "StaticSourcesSupport": "enable",
                }
            },
        )


class TestSecurityGroup:
    """Req 1.6: Security group has correct inbound UDP and SSH rules, all outbound."""

    def test_inbound_udp_rule(self):
        template = _synth()
        template.has_resource_properties(
            "AWS::EC2::SecurityGroup",
            {
                "SecurityGroupIngress": Match.array_with(
                    [
                        Match.object_like(
                            {
                                "IpProtocol": "udp",
                                "FromPort": 5001,
                                "ToPort": 5001,
                            }
                        ),
                    ]
                ),
            },
        )

    def test_inbound_ssh_rule(self):
        template = _synth()
        template.has_resource_properties(
            "AWS::EC2::SecurityGroup",
            {
                "SecurityGroupIngress": Match.array_with(
                    [
                        Match.object_like(
                            {
                                "IpProtocol": "tcp",
                                "FromPort": 22,
                                "ToPort": 22,
                            }
                        ),
                    ]
                ),
            },
        )

    def test_all_outbound_rule(self):
        template = _synth()
        template.has_resource_properties(
            "AWS::EC2::SecurityGroup",
            {
                "SecurityGroupEgress": Match.array_with(
                    [
                        Match.object_like(
                            {
                                "IpProtocol": "-1",
                                "CidrIp": "0.0.0.0/0",
                            }
                        ),
                    ]
                ),
            },
        )


class TestUserData:
    """Req 3.3, 3.4: User data contains chrony PTP and tool installation."""

    def test_chrony_ptp_configuration(self):
        template = _synth()
        user_data = _get_user_data_script(template)
        assert "/dev/ptp_ena" in user_data, "User data must configure PTP via /dev/ptp_ena"
        assert "chrony" in user_data.lower(), "User data must configure chrony"
        assert "phc_enable=1" in user_data, "User data must enable PHC in the ENA driver"

    def test_ethtool_verification_present(self):
        """User data must log ethtool -T output to confirm HW timestamping capability."""
        template = _synth()
        user_data = _get_user_data_script(template)
        assert "ethtool -T" in user_data

    def test_sockperf_installation(self):
        template = _synth()
        user_data = _get_user_data_script(template)
        assert "sockperf" in user_data, "User data must install sockperf"

    def test_iperf3_installation(self):
        template = _synth()
        user_data = _get_user_data_script(template)
        assert "iperf3" in user_data, "User data must install iperf3"


class TestPhcInstanceValidation:
    """Receiver-side HW RX timestamping requires a PHC-capable instance family."""

    @pytest.mark.parametrize("instance_type", [
        "m7i.large",
        "m7a.large",
        "m7g.medium",
        "c7i.large",
        "c7g.large",
        "r7i.large",
        "r7iz.large",
        "i8g.large",
        "c8g.medium",
        "m8g.medium",
        "x8g.medium",
    ])
    def test_phc_capable_families_accepted(self, instance_type):
        # Should synthesize without error
        _synth(instance_type=instance_type)

    @pytest.mark.parametrize("instance_type", [
        "c6in.large",   # previous default — pre-PHC
        "m5.large",
        "c5n.xlarge",
        "t3.medium",
        "m6i.large",
    ])
    def test_non_phc_families_rejected(self, instance_type):
        with pytest.raises(ValueError, match="PHC-capable"):
            _synth(instance_type=instance_type)

    def test_default_instance_type_is_phc_capable(self):
        """Default instance type must be a PHC-capable family."""
        # Synth with no instance_type override — should use the new PHC-capable default
        _synth()


class TestSubscriberLimit:
    """Req 3.6: num_subscribers=257 raises synthesis error."""

    def test_exceeding_256_raises_value_error(self):
        with pytest.raises(ValueError, match="256"):
            _synth(num_subscribers=257)


class TestBaseAmi:
    """Req 5.2, 5.3: base_ami parameter is used when provided."""

    def test_custom_ami_used(self):
        template = _synth(base_ami="ami-test123")
        template.has_resource_properties(
            "AWS::EC2::Instance",
            {"ImageId": "ami-test123"},
        )
