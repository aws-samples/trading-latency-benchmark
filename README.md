# Network Latency Test Stack

This repository contains a network latency test stack that consists of Java based trading client and Ansible playbooks to coordinate 
distributed tests.

Java based trading client is designed to send limit and cancel orders,
allowing you to measure round-trip times of the network communication.

This repository does not contain a trading matching engine.

## Table of Contents

- [Introduction](#introduction)
- [Prerequisites](#prerequisites)
- [Getting Started](#getting-started)
- [Ansible Setup](#ansible-setup)
- [Java Client Setup](#java-client-setup)
- [Usage](#usage)
- [Contributing](#contributing)
- [License](#license)

## Prerequisites
Before you can use this network latency test stack, you'll need to ensure that you have the following prerequisites in place:

**Ansible**: Make sure Ansible is installed on your system. You can download it [here](https://www.ansible.com/).

**Java Development Kit (JDK)**: You'll need a JDK installed on your machine to compile and run the Java client. You can download the JDK from [OpenJDK](https://adoptopenjdk.net/) or [Oracle](https://www.oracle.com/java/).


## Getting Started
### Ansible Setup

Navigate to the Ansible directory:
```
cd deployment/ansible/inventory.yml
```

Edit the `exchange_client_inventory.aws_ec2.yml` file to specify the target instances for your network latency test.
and `exchange_server_inventory.aws_ec2.yml`
As an Ansible inventory we used `aws_ec2` plugin. Therefore we mainly configure 3 properties within this plugin;
1. boto_profile: profile mapping that is defined in ~/.aws/credentials
2. regions: aws region that we use to run tests
3. include_filters: EC2 instance names, plugin fetches public ip address of those instances and run ansible playbooks on only those EC2 instances with provided names

Under the `deployment` directory we have some useful shell scripts that are self explanatory. In order to use them we need to edit those files and change ssh key pair path.
For example `cleanup_exchange_clients.sh` file contains
```bash
ansible-playbook cleanup_all_clients.yaml --key-file  replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
```
replace_me_with_ssh_key_pair should be key-file that is used to connect EC2 instances. 

Here below you can find the list of shell scripts and their usage:

1. `clean_up_exchange_clients.sh`: removes histogram files from previous run, cleans output log, removes previous config history
2. `deploy.sh`: deploys both client app and configuration
3. `latency_report.sh`: runs jar file takes histogram file as an input and generates latency report
4. `tune.sh`: optimizes Operating System for lower latency
5. `restart_exchange_clients.sh`: Stops all running jar processes in the client EC2 instances and starts client processes again
6. `run.sh`: Run script that is used to run client application. This script is deployed to target client EC2 instances and contains optimum JVM parameters
7. `show_latency_reports.sh`: downloads all histogram files from remote EC2 instances and generates latency report
8. `stop.sh`: Stop script that is used to stop java processes on EC2 instances
9. `stop_exchange_clients.sh`: Stops all client processes on all EC2 instances

### Java Client Setup

From the project root directory run following
```
mvn clean install
```

This will produce `./target/ExchangeFlow-1.0-SNAPSHOT.jar` jar file. This is jar file we use as a client process to generate orders as well as to generate human readable reports from histogram files.


## Usage
Once you have set up both Ansible, Maven and ansible inventory files and edit shell scripts to point out correct ssh key pairs to access EC2 instances. 
You can do following

1. Create client jar file using `mvn clean install` 
2. Run deploy.sh to deploy client jar file and client config
3. Run `restart_exchange_clients.sh` to start trading clients in parallel
4. Once test is run for desired duration, run `stop_exchange_clients.sh` to stop all exchange clients
5. Run `show_latency_reports.sh` to download histogram files and generate a latency report

## Optimization used for Java Client

Testing was conducted on Amazon EC2 c6id.metal instances running Amazon Linux. Various application layer various optimisations and techniques were implemented for the HFT client:

### Thread processor affinity via CPU core pinning

Each time a thread is assigned to a core the processor copies thread's data and instructions into its cache. Since this affects latency, one of the optimization technique that is used in this space is core pinning. Once enabled operating system ensures that given thread executes only on the assigned cores which then prevents that unnecessary copy operation.

Our HFT client library uses Netty as underlying networking framework therefore to enable this feature followings are done;

Add AffinityThreadFactory library to the classpath

```xml
<dependency>
    <groupId>net.openhft</groupId>
    <artifactId>affinity</artifactId>
    <version>3.0.6</version>
</dependency>
```

Create thread factories for business logic and io operations seperately.

```java
private static final ThreadFactory NETTY_IO_THREAD_FACTORY = new AffinityThreadFactory("netty-io", AffinityStrategies.DIFFERENT_CORE);

private static final ThreadFactory NETTY_WORKER_THREAD_FACTORY = new AffinityThreadFactory("netty-worker", AffinityStrategies.DIFFERENT_CORE);
```

### Composite buffers to avoid unnecessary buffer copies

Composite buffer is a virtual buffer in Netty which is used to reduce unnecessary object allocations and copy operations during merging multiple frames of data together. For example;

```java
Unpooled.wrappedBuffer(
    ExchangeProtocolImpl.HEADER,
    pair.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.SYMBOL_END,
    clientId.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.CLIENT_ID_END,
    ExchangeProtocolImpl.buySide, ExchangeProtocolImpl.SIDE_END,
    ExchangeProtocolImpl.dummyType, ExchangeProtocolImpl.TYPE_END,
    ExchangeProtocolImpl.dummyBuyPrice, ExchangeProtocolImpl.PRICE_END,
    ExchangeProtocolImpl.dummyAmount, ExchangeProtocolImpl.AMOUNT_END,
    ExchangeProtocolImpl.dummyTimeInForce, ExchangeProtocolImpl.TIME_IN_FORCE_END
)
```

This exposes single ByteBuf API that Netty uses to send messages whereas underlying data can consist of several different bytes, or ByteBuf objects.

###  IO_uring - async IO interface for Linux

io_uring is an asynchronous I/O interface buit for linux kernel. It's a ring buffer in shared memory that are used as a queue between application user space and kernel space. Application puts messages to the submission queue and consumes responses from the completition queue.

Netty is fully async networking library and uses EventLoop mechanism to achieve that. To adapt IO_URING feature Netty has created spesific EventLoop implementation which provides seamless integration with underlying OS kernel. To do so we implemented following - notice the interfaces are the same;
```java
this.nettyIOGroup = USE_IOURING ? new IOUringEventLoopGroup(NETTY_THREAD_COUNT, NETTY_IO_THREAD_FACTORY) : new NioEventLoopGroup(NETTY_THREAD_COUNT, NETTY_IO_THREAD_FACTORY);

this.workerGroup = USE_IOURING ? new IOUringEventLoopGroup(NETTY_THREAD_COUNT, NETTY_WORKER_THREAD_FACTORY) : new NioEventLoopGroup(NETTY_THREAD_COUNT, NETTY_WORKER_THREAD_FACTORY);
```

### Logging HDR histogram records of round trip latencies in execution threads distinct from IO threads

Single responsiblity is common technique that helps make applications modular and re-usable. However that the same technique can also help making applications faster as well. For example in this application we have 2 seperate responsibilities, first is network IO layer and second is measuring round trip latencies and accumulating results in HDR histograms. Second part is the business logic which can be expensive and shouldn't keep network IO threads busy. Therefore network IO threads only receives and sends messages while also putting timestamps on them and worker threads calculates round trip times and save HDR histograms to the disk. In the code snippet below workerGroup is the worker event group that is given to the pipeline implicitly. That forces business logic handler to run on worker event loop.

```java
ChannelPipeline pipeline = channel.pipeline();
pipeline.addLast("http-codec", new HttpClientCodec());
pipeline.addLast("aggregator", new HttpObjectAggregator(65536));
pipeline.addLast(workerGroup, "ws-handler", handler);
```

### HDR Histogram accumulation on disk and aggregation in separate processes

Because we tested such high rates of message per second, HDR Histogram can grow quite fast and consume a lot of memory. To prevent that we utilized HistogramLogWriter to keep intermediate results in the disk and finally merged using HistogramLogReader to get the final report in the seperate process. Therefore application main function supports 2 commands one is latency test and second is latency report generation which takes histogram file as an input and produces percentiles as an output.

### The JVM was tuned to reduce GC pressure

Before starting tests we warmed up JVM processes by sending/receiving orders without measuring time. This enables JIT compiler to compile and optimize the code during runtime. Also we restarted processes every hour to reduce memory defragmentation. Since HDR is accumulative this can be done without effecting results. Also to reduce GC pressure following JVM parameters are applied at the client level;

```bash
java -Xms16g -Xmx16g -XX:ConcGCThreads=32 -XX:+UseTransparentHugePages \
-XX:+UnlockExperimentalVMOptions -XX:+UseZGC -XX:+TieredCompilation -XX:+UseLargePages \
-XX:LargePageSizeInBytes=2m -XX:+UnlockDiagnosticVMOptions \
-XX:+DoEscapeAnalysis -XX:+UseCompressedOops -XX:+UseTLAB \
-XX:+UseCompressedOops -XX:InitiatingHeapOccupancyPercent=60 \
-XX:+UseNUMA \
-server -Dsun.rmi.dgc.server.gcInterval=0x7FFFFFFFFFFFFFFE \
-Dsun.rmi.dgc.client.gcInterval=0x7FFFFFFFFFFFFFFE -Dfile.encoding=UTF \
-jar ExchangeFlow-1.0-SNAPSHOT.jar latency-test
```

### Lock-free libraries implemented like LMAX RingBuffer

We implemented 2 different versions for this application. First version waits for matching engine acks and then reacts to it, the other version doesn't wait for acks and keeps seending orders in parallel. The second version was using LMAX RingBuffer to achieve that. Since this has created too many parallel flows we noticed that this is not the real world behaviour. Even though we were able to observe benefits of using LMAX RingBuffer to achieve greater performance benefits we sticked to initial version where we don't use LMAX RingBuffer as real life business use case fits better to first version.

In the interests of maintaining a simple, vanilla, baseline many additional stack optimisations that are typically implemented for specific workload types were NOT applied for this testing (IRQ handling, CPU P-state and C-state controls, network buffers, kernel bypass, receive side scaling, transmit packet steering, Linux scheduler policies and AWS Elastic Network Adapter tuning). Results could be improved by more closely visiting a combination of these areas to further tune the testing setup.

## Security

See [CONTRIBUTING](CONTRIBUTING.md#security-issue-notifications) for more information.

## License

This library is licensed under the MIT-0 License. See the LICENSE file.

