#!/usr/bin/env node
import 'source-map-support/register';
import * as cdk from 'aws-cdk-lib';
import { TradingBenchmarkMultiAzStack } from '../lib/multi-az-stack';
import { TradingBenchmarkSingleInstanceStack } from "../lib/single-instance-stack";
import { TradingBenchmarkClusterPlacementGroupStack } from "../lib/cluster-placement-group-stack";
import { TradingBenchmarkAmiBuilderStack } from "../lib/ami-builder-stack";
import { LatencyHuntingStack } from "../lib/latency-hunting-stack";
import { LatencyHuntingBYOVPCStack } from "../lib/latency-hunting-byovpc-stack";

const app = new cdk.App();

// Get command line arguments
const deploymentType = app.node.tryGetContext('deploymentType') || 'single';
const clientInstanceType = app.node.tryGetContext('clientInstanceType');
const serverInstanceType = app.node.tryGetContext('serverInstanceType');
const instanceType1 = app.node.tryGetContext('instanceType1');
const instanceType2 = app.node.tryGetContext('instanceType2');
const instanceType = app.node.tryGetContext('instanceType');
const baseAmi = app.node.tryGetContext('baseAmi');
const keyPairName = app.node.tryGetContext('keyPairName');
const region = app.node.tryGetContext('region') || 'us-east-1';
const vpcCidr = app.node.tryGetContext('vpcCidr');
const vpcId = app.node.tryGetContext('vpcId');
const subnetId = app.node.tryGetContext('subnetId');
const securityGroupId = app.node.tryGetContext('securityGroupId');
const useExistingVpc = app.node.tryGetContext('useExistingVpc');
const availabilityZone = app.node.tryGetContext('availabilityZone');
const elasticIpsString = app.node.tryGetContext('elasticIps');

// Parse elastic IPs from comma-separated string to array
const elasticIps = elasticIpsString ? elasticIpsString.split(',').map((eip: string) => eip.trim()) : undefined;

// Environment configuration
// Note: For VPC lookup to work, we need explicit account and region
const env = {
  account: process.env.CDK_DEFAULT_ACCOUNT,
  region: region
};

// Deploy the appropriate stack based on the deployment type
switch (deploymentType.toLowerCase()) {
  case 'ami-builder':
    console.log('Deploying Trading Benchmark AMI Builder Stack');
    new TradingBenchmarkAmiBuilderStack(app, 'TradingBenchmarkAmiBuilderStack', {
      env,
      instanceType,
      baseAmi,
      keyPairName
    });
    break;

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

  case 'latency-hunting':
  case 'hunting':
    if (useExistingVpc === 'true' || useExistingVpc === true) {
      // Use BYOVPC stack - requires vpcId and subnetId
      console.log('Deploying EC2 Hunting BYOVPC Stack (using existing VPC)');
      if (!vpcId) {
        throw new Error('vpcId is required when using existing VPC. Provide --vpc-id parameter.');
      }
      if (!subnetId) {
        throw new Error('subnetId is required when using existing VPC. Provide --subnet-id parameter.');
      }
      if (!keyPairName) {
        throw new Error('keyPairName is required. Provide --key-pair parameter.');
      }
      
      new LatencyHuntingBYOVPCStack(app, 'LatencyHuntingBYOVPCStack', {
        env,
        vpcId: vpcId,
        subnetId: subnetId,
        keyPairName: keyPairName,
        securityGroupId: securityGroupId,
        elasticIps: elasticIps
      });
    } else {
      // Use original stack that can create or import VPC
      console.log('Deploying EC2 Hunting Stack for network placement optimization');
      new LatencyHuntingStack(app, 'LatencyHuntingStack', {
        env,
        keyPairName: keyPairName || 'virginia',
        vpcCidr,
        vpcId,
        elasticIps: elasticIps
      });
    }
    break;

  case 'single':
  default:
    console.log('Deploying Trading Benchmark Single Instance Stack');
    new TradingBenchmarkSingleInstanceStack(app, 'TradingBenchmarkSingleInstanceStack', {
      env,
      instanceType1,
      instanceType2,
      vpcCidr,
      keyPairName,
      availabilityZone
    });
    break;
}
