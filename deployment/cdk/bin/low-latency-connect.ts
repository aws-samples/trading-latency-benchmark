#!/usr/bin/env node
import 'source-map-support/register';
import * as cdk from 'aws-cdk-lib';
import {LowLatencyConnectStack} from '../lib/low-latency-connect-stack';
import {SingleMetalInstanceStack} from "../lib/single-metal-instance-stack";

const app = new cdk.App();
new LowLatencyConnectStack(app, 'LowLatencyConnectStack', {
  env: {
    region: 'us-east-1',
  },
});
// new SingleMetalInstanceStack(app, 'SingleMetalInstanceStack', {
//     env: {
//         region: 'us-east-1'
//     }
// });
