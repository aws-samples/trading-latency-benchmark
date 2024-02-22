# Network Latency Test Stack

This repository contains a network latency test stack that consists of Java based trading client and Ansible playbooks to coordinate 
distributed tests.

Java based trading client is designed to send limit and cancel orders,
allowing you to measure round-trip times of the network communication.

This repository does not contain a real trading matching engine but only mocks it.

## Table of Contents

- [Introduction](#introduction)
- [Prerequisites](#prerequisites)
- [Getting Started](#getting-started)
- [Deployment with Ansible](#deployment-with-ansible)
- [Start Running Tests](#start-running-tests)
- [Fetch and Analyze Logs](#fetch-and-analyze-logs)
- [Generating Self Signed Certificates for Testing SSL connections](#generating-self-signed-certificates-for-testing-ssl-connections)
- [Optimization used for Java Client](#optimization-used-for-java-client)
- [Contributing](#contributing)
- [License](#license)

## Prerequisites
Before you can use this network latency test stack, you'll need to ensure that you have the following prerequisites in place:

**Ansible**: Make sure Ansible is installed on your system. You can download it [here](https://www.ansible.com/).

**Java Development Kit (JDK)**: You'll need a JDK installed on your machine to compile and run the Java client. You can download the JDK from [OpenJDK](https://adoptopenjdk.net/) or [Oracle](https://www.oracle.com/java/).


## Getting Started
### Deployment with Ansible

1. Generate SSH key pairs for the instances
2. Update `.aws_ec2.yml` inventory files under `deployment/ansible/inventory` with your EC2 instance names. As an example you can find 2 inventories one for tokyo one for virginia.
3. Open `deploy.sh` file and update `INVENTORY` and `SSH_KEY_FILE`. `SSH_KEY_FILE` is the ssh key pair that you use to connect to EC2 instances.
4. Run `deploy.sh`

The `deploy.sh` script will:

1. Build the Java application using Maven
2. Run Ansible playbooks to:
    - Install Java on EC2 instances
    - Deploy the client application artifacts
    - Deploy the client configuration
    - Deploy the mock trading server

The playbooks use the AWS EC2 dynamic inventory to target the instances for example virginia_inventory.aws_ec2.yml.
`deploy_mock_trading_server.yaml` playbook defines a set of tasks to:
- Copy over the Rust source code
- Install build dependencies like GCC, Make etc
- Download and install Rust/Cargo
- Build the trading server release artifact
- [Generate self-signed SSL certificates. You can also do this manually as described below](#generating-self-signed-certificates-for-testing-ssl-connections).
- Configure the server to use SSL
- Deploy the configuration file

### Start Running Tests

`start_latency_test.yaml` playbook is used to start the exchange server and client processes for performance testing on AWS EC2 instances.

The playbook defines tasks to:

    Start the exchange server process in screen on the server instances
    Start the exchange client processes in screen on the client instances

Steps:

    Ensure server and client code is deployed
    Update EC2 inventory file with tags
    Run ansible-playbook restart.yml

You can monitor client logs from `/home/ec2-user/output.log`and server logs from `/home/ec2-user/mock-trading-server/target/release/output.log`

### Fetch and Analyze Logs

`deployment/show_latency_reports.sh` script fetches latency histogram logs from EC2 instances and analyzes them locally.
Usage

1. Open show_latency_reports.sh`
2. set INVENTORY 
3. set SSH_KEY_FILE
4. run show_latency_reports.sh

Workflow

The script performs the following steps:

    Runs the fetch_histogram_logs.yaml Ansible playbook to copy logs from instances
    Loops through fetched log files
    Calls a Java program to analyze each log
    The program outputs latency reports

### Generating Self Signed Certificates for Testing SSL connections

1. Generate a 2048-bit RSA private key (localhost.key) and a Certificate Signing Request (localhost.csr) using OpenSSL. The private key is kept secret and is used to digitally sign documents. 
The CSR contains information about the key and identity of the requestor and is used to apply for a certificate.
```bash
openssl genrsa -out localhost.key 2048
openssl req -new -key localhost.key -out localhost.csr
```

2. Self-sign the CSR to generate a localhost test certificate (localhost.crt) that is valid for 365 days. 
This acts as a certificate authority to sign our own certificate for testing SSL connections.
```bash
openssl x509 -req -days 365 -in localhost.csr -signkey localhost.key -out localhost.crt 
```

3. Export the private key and self-signed certificate to a PKCS#12 keystore (keystore.p12). This bundles the private key and certificate together in a format usable by Java high-frequency trading (HFT) client. 
A password protects the keystore.
```bash
openssl pkcs12 -export -out keystore.p12 -inkey localhost.key -in localhost.crt
```

4. Configure the Java high-frequency trading (HFT) client to use the keystore for SSL connections by setting USE_SSL=true 
and providing the KEY_STORE_PATH and KEY_STORE_PASSWORD in the `config.properties` file.
```
USE_SSL=true
KEY_STORE_PATH=keystore.p12
KEY_STORE_PASSWORD=YOUR_PASSWORD
```

5. Similarly, configure the Rust mock exchange server to use the localhost.key for its private key and the localhost.crt for its certificate chain. 
Enable SSL usage by setting use_ssl=true in its `configuration.toml` file.
```
private_key = "/path/to/localhost.key"
cert_chain = "/path/to/localhost.crt"
use_ssl = true
```

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

