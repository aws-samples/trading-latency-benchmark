# AWS Low Latency Stack

This AWS Cloud Development Kit (AWS CDK) to create instances in each AZs us-east-1. T
The use case for this is to run ping clients from each AZs to detect which AZ is closest to the endpoint or which endpoint closest to the region.
Therefore stack provisions a VPC with public subnets across multiple Availability Zones and launches an Amazon EC2 instance of type `r4.xlarge` in each Availability Zone.

## Prerequisites

- AWS account
- AWS CLI
- Node.js >=14.x
- AWS CDK

## Project Structure

The main code resides in the `lib/low-latency-connect-stack.ts` file, which defines the `LowLatencyConnectStack` class that extends `cdk.Stack`.

## Deployment

1. Install the required dependencies:
```
npm install
```

2. Bootstrap the AWS environment (only required the first time):
```
cdk bootstrap
```

3. Deploy the stack:
```bash
cdk deploy
```


## Architecture

The deployment creates the following resources:

- **VPC**: A Virtual Private Cloud (VPC) with six public subnets, one in each of the six Availability Zones in the `us-east-1` region.
- **Security Group**: A security group allowing inbound SSH traffic from any IPv4 address.
- **EC2 Instances**: Six Amazon EC2 instances of type `r4.xlarge` are launched, one in each public subnet. The instances use Amazon Linux 2 as the operating system and have a 100 GB EBS volume attached.

## Cleanup

To remove the resources created by this stack:

```bash
cdk destroy
```

## License
This library is licensed under the MIT-0 License. See the LICENSE file.
