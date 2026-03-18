#!/usr/bin/env node
import 'source-map-support/register';
import * as cdk from 'aws-cdk-lib';
import { TradingBenchmarkMultiAzStack } from '../lib/multi-az-stack';
import { TradingBenchmarkSingleInstanceStack } from "../lib/single-instance-stack";
import { TradingBenchmarkClusterPlacementGroupStack } from "../lib/cluster-placement-group-stack";
import { TradingBenchmarkAmiBuilderStack } from "../lib/ami-builder-stack";
import { LatencyHuntingStack } from "../lib/latency-hunting-stack";
import { LatencyHuntingBYOVPCStack } from "../lib/latency-hunting-byovpc-stack";
import { FeederStack, SubscriberStack, PeeringStack } from "../lib/feeder-stack";

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
const singleEc2Instance = app.node.tryGetContext('singleEc2Instance');
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
    // TradingBenchmarkCluster refers to single AZ, two EC2 instance (Trading-Server, Trading-Client), within CPG
    console.log('Deploying Trading Benchmark Cluster Placement Group Stack');
    new TradingBenchmarkClusterPlacementGroupStack(app, 'TradingBenchmarkClusterPlacementGroupStack', {
      env,
      clientInstanceType,
      serverInstanceType
    });
    break;

  case 'multi-az':
    // TradingBenchmarkMultiAz refers to multiple AZ, two EC2 instance (Trading-Server, Trading-Client), no CPG
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

  case 'feeder':
  case 'remote-feeder': {
    console.log('Deploying Feed Handler POC Stacks (feeder region: London, subscriber region: Frankfurt)');
    if (!keyPairName) {
      throw new Error('keyPairName is required for feeder stack. Provide --context keyPairName=<name>');
    }
    const feederRegion          = app.node.tryGetContext('feederRegion')          || region || 'eu-west-2';
    const subscriberRegion      = app.node.tryGetContext('subscriberRegion')      || 'eu-central-1';
    const subscriberKeyPairName = app.node.tryGetContext('subscriberKeyPairName') || keyPairName;
    const feederAmiId           = app.node.tryGetContext('feederAmiId');
    const subscriberAmiId       = app.node.tryGetContext('subscriberAmiId');
    const subscriberCount       = app.node.tryGetContext('subscriberCount');
    const multicastGroup        = app.node.tryGetContext('multicastGroup');
    const dataPort              = app.node.tryGetContext('dataPort');
    const ctrlPort              = app.node.tryGetContext('ctrlPort');
    const sourceFeederCidr      = app.node.tryGetContext('sourceFeederCidr');
    const feederCidr            = app.node.tryGetContext('feederCidr');
    const feederVpcCidr         = app.node.tryGetContext('feederVpcCidr')         || vpcCidr;
    const subscriberVpcCidr     = app.node.tryGetContext('subscriberVpcCidr');
    const dataPortNum           = dataPort ? parseInt(dataPort, 10) : undefined;

    const feederStack = new FeederStack(app, 'FeederStack', {
      env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: feederRegion },
      keyPairName,
      amiId:                feederAmiId,
      feederInstanceType:   app.node.tryGetContext('feederInstanceType'),
      exchangeInstanceType: app.node.tryGetContext('exchangeInstanceType'),
      vpcCidr:              feederVpcCidr,
      multicastGroup:       multicastGroup || undefined,
      dataPort:             dataPortNum,
      ctrlPort:             ctrlPort ? parseInt(ctrlPort, 10) : undefined,
      sourceFeederCidr:     sourceFeederCidr || undefined,
    });

    new SubscriberStack(app, 'SubscriberStack', {
      env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: subscriberRegion },
      keyPairName:            subscriberKeyPairName,
      amiId:                  subscriberAmiId,
      subscriberInstanceType: app.node.tryGetContext('subscriberInstanceType'),
      subscriberCount:        subscriberCount ? parseInt(subscriberCount, 10) : undefined,
      vpcCidr:                subscriberVpcCidr,
      dataPort:               dataPortNum,
      feederCidr:             feederCidr || undefined,
    });

    new PeeringStack(app, 'PeeringStack', {
      env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: feederRegion },
      feederVpc:          feederStack.vpc,
      feederVpcCidr:      feederStack.vpcCidr,
      feederSg:           feederStack.sg,
      subscriberStackName: 'SubscriberStack',
      subscriberRegion,
      subscriberVpcCidr:  subscriberVpcCidr || '10.62.0.0/16',
      dataPort:           dataPortNum ?? 5000,
    });
    break;
  }

  case 'single':
  default:
    // SingleInstance refers to single AZ, two EC2 instance (Trading-Server, Trading-Client), no CPG
    console.log('Deploying Trading Benchmark Single Instance Stack');
    new TradingBenchmarkSingleInstanceStack(app, 'TradingBenchmarkSingleInstanceStack', {
      env,
      instanceType1,
      instanceType2,
      vpcCidr,
      keyPairName,
      availabilityZone,
      singleEc2Instance: singleEc2Instance === 'true' || singleEc2Instance === true,
      baseAmi
    });
    break;
}
