#!/usr/bin/env python3
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
import os
import aws_cdk as cdk

from stacks.clock_bound_stack import ClockBoundMeasureStack

app = cdk.App()

region = app.node.try_get_context("region") or os.getenv("CDK_DEFAULT_REGION")
account = os.getenv("CDK_DEFAULT_ACCOUNT")

ClockBoundMeasureStack(
    app,
    "ClockBoundMeasureStack",
    env=cdk.Environment(
        account=account,
        region=region,
    ),
)

app.synth()
