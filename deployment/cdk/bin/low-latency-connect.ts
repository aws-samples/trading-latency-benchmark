#!/usr/bin/env node
import 'source-map-support/register';
import * as cdk from 'aws-cdk-lib';
import {LowLatencyConnectStack} from '../lib/low-latency-connect-stack';
import {SingleInstanceStack} from "../lib/single-instance-stack";

const app = new cdk.App();
new SingleInstanceStack(app, 'SingleInstanceStack', {
    env: {
        region: 'us-east-1'
    }
});
