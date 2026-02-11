# AWS EC2 Network Latency Benchmark for Trading Applications

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build Status](https://github.com/aws-samples/trading-latency-benchmark/workflows/Build%20Pull%20Request/badge.svg)](https://github.com/aws-samples/trading-latency-benchmark/actions)
[![AWS](https://img.shields.io/badge/AWS-%23FF9900.svg?style=flat&logo=amazon-aws&logoColor=white)](https://aws.amazon.com/)
[![Java](https://img.shields.io/badge/Java-11+-blue.svg)](https://openjdk.java.net/)
[![Rust](https://img.shields.io/badge/Rust-1.70+-orange.svg)](https://www.rust-lang.org/)

This repository contains a comprehensive network latency benchmarking solution designed specifically for trading applications running on AWS EC2. The benchmark suite measures round-trip latency between trading clients and servers, providing valuable insights for latency-sensitive financial applications.

## Project Overview

### Purpose
The primary goal of this project is to measure and analyze network latency in a simulated trading environment on AWS EC2 instances. In high-frequency trading (HFT) applications, even microseconds of latency can significantly impact trading outcomes and profitability. This benchmark helps:

- Evaluate the network performance of different EC2 instance types for trading workloads
- Measure the impact of various system and JVM optimizations on latency
- Provide data-driven insights for architecting low-latency trading systems on AWS
- Compare performance across different AWS regions and placement groups
- Identify bottlenecks and optimization opportunities in trading infrastructure

### Why This Matters
Financial markets operate at extremely high speeds, where being just a few microseconds faster than competitors can mean the difference between profit and loss. This benchmark suite allows you to:

1. Make informed decisions about EC2 instance selection for trading applications
2. Understand the real-world impact of system-level tuning on latency
3. Quantify the performance benefits of various optimization techniques
4. Establish baseline performance metrics for your trading infrastructure
5. Test the impact of network conditions on trading application performance

### Components

The benchmark suite consists of:

1. **Java Trading Client**: A high-performance client that sends limit and cancel orders and measures round-trip times
2. **Rust Mock Trading Server**: A lightweight server that simulates a trading exchange by responding to client orders
3. **CDK Infrastructure**: AWS CDK code to deploy the required EC2 instances and networking components
4. **Ansible Playbooks**: Scripts to provision instances, run tests, and collect results
5. **OS-Tuned AMI Builder**: Automated pipeline to create pre-optimized Amazon Machine Images with performance tuning baked in
6. **Analysis Tools**: Utilities to process and visualize latency data using HDR Histograms

### Sequence Diagram
The benchmark contains a simple HFT client and Matching Engine written in Java to simulate a basic order flow sequence for latency measurements, as per the following diagram:

![HFT Client Sequence Diagram](assets/images/hft-client-sequence-diagram.png)

## Prerequisites

Before using this benchmark suite, ensure you have the following prerequisites:

- **AWS CLI**: Configured with appropriate credentials and default region
- **AWS CDK**: Installed and bootstrapped in your AWS account
- **Ansible**: Version 2.9+ installed on your local machine
- **SSH Key Pair**: Generated and registered with AWS for EC2 instance access

Set the `SSH_KEY_FILE` environment variable to point to your key:
```bash
export SSH_KEY_FILE=~/.ssh/virginia.pem
```

## Getting Started

### Option A: Quick Start with Pre-Built OS-Tuned AMI (Recommended)

For fastest deployment and optimal performance, build a pre-tuned AMI first:

```bash
cd deployment
./build-tuned-ami.sh --key-file $SSH_KEY_FILE
```

This creates an AMI with all OS-level optimizations pre-applied (CPU isolation, network tuning, hugepages, etc.). The process takes ~20-30 minutes but eliminates the need to run OS tuning on every deployment.

**Important - CPU Allocation Considerations:**
- CPU isolation settings are **baked into the AMI** at build time based on the builder instance's vCPU count
- For optimal performance, build the AMI on the same instance type you plan to deploy to (or within the same size class)
- Example: Building on c7i.4xlarge (16 vCPUs) then deploying on c7i.48xlarge (192 vCPUs) will only isolate 12 cores instead of 176
- See the [AMI Builder README](deployment/AMI_BUILDER_README.md#building-amis-for-multiple-instance-types) for recommended build strategies

Then deploy using the tuned AMI:

```bash
cd cdk
cdk deploy --context deploymentType=cluster --context baseAmi=ami-xxxxxxxxx
```

See [deployment/AMI_BUILDER_README.md](deployment/AMI_BUILDER_README.md) for detailed AMI builder documentation.

### Option B: Standard Deployment with Manual OS Tuning

### 1. Deploy Infrastructure with CDK

Deploy the required AWS infrastructure using CDK. You have several deployment options:

#### Default Single Instance Deployment
Create SSH keypair manually and name it as for example frankfurt then

```bash
cd deployment/cdk
npm install
cdk deploy --context region=eu-central-1 --context availabilityZone=eu-central-1a --context keyPairName=frankfurt --context instanceType1=c7i.4xlarge --context instanceType2=c6in.4xlarge
```

#### Client-Server Architecture with Cluster Placement Group

For optimal network performance between client and server, deploy them in a Cluster Placement Group:

```bash
cd deployment/cdk
npm install
cdk deploy --context deploymentType=cluster
```

You can also specify instance types for client and server:

```bash
cdk deploy --context deploymentType=cluster --context clientInstanceType=c7i.4xlarge --context serverInstanceType=c6in.4xlarge
```

#### Multi-AZ Deployment

To test latency across multiple availability zones:

```bash
cd deployment/cdk
npm install
cdk deploy --context deploymentType=multi-az
```

#### AMI Builder Deployment

To build an OS-tuned AMI for reuse across deployments:

```bash
cd deployment/cdk
npm install
cdk deploy --context deploymentType=ami-builder --context instanceType=c7i.4xlarge
```

Or use the automated build script (recommended):

```bash
cd deployment
./build-tuned-ami.sh --instance-type c7i.4xlarge --key-file $SSH_KEY_FILE
```


### 2. Run the Benchmark Tests

After deploying the infrastructure, use the following Ansible playbooks to run the benchmark tests:

```bash
cd ../ansible

# Provision EC2 instances, and deploy both client and server applications
ansible-playbook provision_ec2.yaml --key-file $SSH_KEY_FILE -i ./inventory/inventory.aws_ec2.yml

# Stop any existing tests
ansible-playbook stop_latency_test.yaml --key-file $SSH_KEY_FILE -i ./inventory/inventory.aws_ec2.yml

# Apply OS-level performance tuning
ansible-playbook tune_os.yaml --key-file $SSH_KEY_FILE -i ./inventory/inventory.aws_ec2.yml

# Start the mock trading server
ansible-playbook restart_mock_trading_server.yaml --key-file $SSH_KEY_FILE -i ./inventory/inventory.aws_ec2.yml

# Start the HFT client
ansible-playbook restart_hft_client.yaml --key-file $SSH_KEY_FILE -i ./inventory/inventory.aws_ec2.yml

# Start the test run for desired duration
ansible-playbook start_latency_test.yaml --key-file $SSH_KEY_FILE -i ./inventory/inventory.aws_ec2.yml

# Let the test run for desired duration, then stop it
ansible-playbook stop_latency_test.yaml --key-file $SSH_KEY_FILE -i ./inventory/inventory.aws_ec2.yml
```

### 3. Collect and Analyze Results

After running the tests, collect and analyze the latency results:

```bash
cd ..
./show_latency_reports.sh --inventory $(PWD)/ansible/inventory/inventory.aws_ec2.yml --key $SSH_KEY_FILE
```

This script will:
- Fetch histogram logs from the EC2 instances
- Process the logs to generate latency reports
- Create a summary report with key latency metrics
## Understanding the Results

The latency reports include several important metrics:

- **Min/Max/Mean Latency**: Basic statistics about the observed latencies
- **Percentile Latencies**: Values at key percentiles (50th, 90th, 99th, 99.9th, etc.)
- **Coordinated Omission Free**: Adjusted metrics that account for coordinated omission
- **Histogram Distribution**: Visual representation of the latency distribution

These metrics help identify not just average performance but also worst-case scenarios that are critical for trading applications.

## Advanced Configuration

### Pre-Built OS-Tuned AMIs

For production deployments, we recommend using pre-built OS-tuned AMIs:

**Benefits:**
- **Faster Deployments**: Skip 10-15 minute OS tuning process on every deployment
- **Consistency**: Guaranteed identical OS optimizations across all instances
- **Immutable Infrastructure**: Version-controlled tuning configurations via AMI tags
- **Dynamic Scaling**: CPU isolation automatically adapts to instance size (2-192 vCPUs supported)

**Build Strategy:**
Build separate AMIs for different instance size classes for optimal performance:
- Small (4-8 vCPUs): Build on c7i.2xlarge
- Medium (16-32 vCPUs): Build on c7i.4xlarge
- Large (48-96 vCPUs): Build on c7i.24xlarge
- X-Large (128-192 vCPUs): Build on c7i.48xlarge

See [deployment/AMI_BUILDER_README.md](deployment/AMI_BUILDER_README.md) for complete documentation.

### Tuning OS Parameters

The `tune_os.yaml` playbook applies various system-level optimizations with **dynamic CPU core allocation**:

**CPU Optimizations:**
- Automatically detects vCPU count and scales housekeeping cores
- Disables hyperthreading, C-states, and P-states
- Sets CPU governor to performance
- Isolates cores for trading applications (scales from 1 to 176 cores)
- Moves IRQs and kernel workqueues to housekeeping cores

**Other Optimizations:**
- Network stack parameters (busy polling, TSO/GSO disabled)
- Memory settings (hugepages, THP disabled, NUMA)
- I/O scheduler configuration
- Kernel parameters

You can customize these settings in the playbook based on your specific requirements.

### JVM Tuning

The Java client is launched with specific JVM parameters to optimize performance. These parameters control:

- Memory allocation and garbage collection
- Thread affinity and scheduling
- JIT compilation behavior
- Memory pre-touch and large pages

### Optimization Techniques

The benchmark implements several optimization techniques commonly used in high-frequency trading applications:

1. **Dynamic CPU Isolation**: Automatically scales isolated cores based on instance size (supports 2-192 vCPUs)
2. **Thread Processor Affinity**: Pins threads to specific CPU cores to prevent cache thrashing
3. **Composite Buffers**: Reduces unnecessary object allocations and copy operations
4. **Separate Execution and IO Threads**: Keeps network I/O threads dedicated to communication
5. **HDR Histogram for Latency Recording**: Efficiently records latency measurements with high precision
6. **io_uring Transport**: Uses Linux io_uring for zero-copy networking when available
7. **OS-Level Tuning**: Network stack, memory management, and I/O scheduler optimizations

## Contributing

See [CONTRIBUTING](CONTRIBUTING.md) for details on how to contribute to this project.

## License

This library is licensed under the MIT-0 License. See the LICENSE file.
