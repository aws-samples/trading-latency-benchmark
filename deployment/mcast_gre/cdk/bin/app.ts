#!/usr/bin/env node
import 'source-map-support/register';
import * as cdk from 'aws-cdk-lib';
import { SingleRegionStack } from '../lib/single-region-stack';
import { SourceStack, SubscriberStack, PeeringStack } from '../lib/cross-region-stack';

const app = new cdk.App();

const deploymentType = app.node.tryGetContext('deploymentType') || 'single-region';
const keyPairName    = app.node.tryGetContext('keyPairName');
const region         = app.node.tryGetContext('region') || 'eu-central-1';

if (!keyPairName) {
  throw new Error('keyPairName context is required. Pass --context keyPairName=<name>');
}

switch (deploymentType.toLowerCase()) {
  case 'cross-region': {
    const sourceRegion           = app.node.tryGetContext('sourceRegion')           || 'eu-west-2';
    const subscriberRegion       = app.node.tryGetContext('subscriberRegion')       || 'eu-central-1';
    const subscriberKeyPairName  = app.node.tryGetContext('subscriberKeyPairName')  || keyPairName;
    const sourceAmiId            = app.node.tryGetContext('sourceAmiId')            || undefined;
    const subscriberAmiId        = app.node.tryGetContext('subscriberAmiId')        || undefined;
    const feederInstanceType     = app.node.tryGetContext('feederInstanceType')     || undefined;
    const exchangeInstanceType   = app.node.tryGetContext('exchangeInstanceType')   || undefined;
    const subscriberInstanceType = app.node.tryGetContext('subscriberInstanceType') || undefined;
    const subscriberCount        = app.node.tryGetContext('subscriberCount');
    const multicastGroup         = app.node.tryGetContext('multicastGroup')         || undefined;
    const dataPort               = app.node.tryGetContext('dataPort');
    const ctrlPort               = app.node.tryGetContext('ctrlPort');
    const sourceFeederCidr       = app.node.tryGetContext('sourceFeederCidr')       || undefined;
    const feederCidr             = app.node.tryGetContext('feederCidr')             || undefined;
    const sourceVpcCidr          = app.node.tryGetContext('sourceVpcCidr')          || '10.61.0.0/16';
    const subscriberVpcCidr      = app.node.tryGetContext('subscriberVpcCidr')      || '10.62.0.0/16';
    const dataPortNum            = dataPort  ? parseInt(dataPort,  10) : undefined;
    const ctrlPortNum            = ctrlPort  ? parseInt(ctrlPort,  10) : undefined;
    const subCountNum            = subscriberCount ? parseInt(subscriberCount, 10) : undefined;

    const feederStack = new SourceStack(app, 'FeederStack', {
      env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: sourceRegion },
      keyPairName,
      amiId:              sourceAmiId,
      feederInstanceType,
      exchangeInstanceType,
      vpcCidr:            sourceVpcCidr,
      multicastGroup,
      dataPort:           dataPortNum,
      ctrlPort:           ctrlPortNum,
      sourceFeederCidr,
    });

    new SubscriberStack(app, 'SubscriberStack', {
      env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: subscriberRegion },
      keyPairName:            subscriberKeyPairName,
      amiId:                  subscriberAmiId,
      subscriberInstanceType,
      subscriberCount:        subCountNum,
      vpcCidr:                subscriberVpcCidr,
      dataPort:               dataPortNum,
      feederCidr,
    });

    new PeeringStack(app, 'PeeringStack', {
      env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: sourceRegion },
      feederVpc:          feederStack.vpc,
      feederVpcCidr:      feederStack.vpcCidr,
      feederSg:           feederStack.sg,
      subscriberStackName: 'SubscriberStack',
      subscriberRegion,
      subscriberVpcCidr,
      dataPort:           dataPortNum ?? 5000,
    });
    break;
  }

  case 'single-region':
  case 'cpg':
  default: {
    const instanceType    = app.node.tryGetContext('instanceType')    || 'c7i.4xlarge';
    const amiId           = app.node.tryGetContext('amiId')           || undefined;
    const subscriberCount = app.node.tryGetContext('subscriberCount');

    new SingleRegionStack(app, 'SingleRegionStack', {
      env: { account: process.env.CDK_DEFAULT_ACCOUNT, region },
      keyPairName,
      instanceType,
      amiId,
      subscriberCount: subscriberCount ? parseInt(subscriberCount, 10) : undefined,
    });
    break;
  }
}
