#!/usr/bin/env node
import 'source-map-support/register';
import * as cdk from 'aws-cdk-lib';
import { TradingBenchmarkMultiAzStack } from '../lib/multi-az-stack';
import { TradingBenchmarkSingleInstanceStack } from "../lib/single-instance-stack";
import { TradingBenchmarkClusterPlacementGroupStack } from "../lib/cluster-placement-group-stack";

const app = new cdk.App();

// Get command line arguments
const deploymentType = app.node.tryGetContext('deploymentType') || 'single';
const clientInstanceType = app.node.tryGetContext('clientInstanceType');
const serverInstanceType = app.node.tryGetContext('serverInstanceType');
const instanceType1 = app.node.tryGetContext('instanceType1');
const instanceType2 = app.node.tryGetContext('instanceType2');
const instanceType = app.node.tryGetContext('instanceType');

// Environment configuration
const env = {
  region: 'us-east-1'
};

// Deploy the appropriate stack based on the deployment type
switch (deploymentType.toLowerCase()) {
  case 'cluster':
  case 'placement-group':
  case 'cpg':
    console.log('Deploying Trading Benchmark Cluster Placement Group Stack');
    new TradingBenchmarkClusterPlacementGroupStack(app, 'TradingBenchmarkClusterPlacementGroupStack', {
      env,
      clientInstanceType,
      serverInstanceType
    });
    break;
  
  case 'multi-az':
    console.log('Deploying Trading Benchmark Multi-AZ Stack');
    new TradingBenchmarkMultiAzStack(app, 'TradingBenchmarkMultiAzStack', {
      env,
      instanceType
    });
    break;
  
  case 'single':
  default:
    console.log('Deploying Trading Benchmark Single Instance Stack');
    new TradingBenchmarkSingleInstanceStack(app, 'TradingBenchmarkSingleInstanceStack', {
      env,
      instanceType1,
      instanceType2
    });
    break;
}
