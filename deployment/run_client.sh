#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

# -Dio.netty.handler.ssl.openssl.engine=qatengine
# Add NUMA binding to separate from server memory
numactl --membind=1 taskset -c 10-15 chrt -f 80 java \
-Xms8g -Xmx8g \
-XX:+AlwaysPreTouch \
-XX:+UseLargePages \
-XX:LargePageSizeInBytes=2m \
-XX:+UnlockExperimentalVMOptions \
-XX:+UseZGC \
-XX:ConcGCThreads=2 \
-XX:ZCollectionInterval=300 \
-XX:+UseNUMA \
-XX:+UnlockDiagnosticVMOptions \
-XX:GuaranteedSafepointInterval=0 \
-XX:+UseCountedLoopSafepoints \
-XX:+DisableExplicitGC \
-XX:+DoEscapeAnalysis \
-XX:+OptimizeStringConcat \
-XX:+UseCompressedOops \
-XX:+UseTLAB \
-XX:+UseThreadPriorities \
-XX:ThreadPriorityPolicy=1 \
-XX:CompileThreshold=1000 \
-XX:+TieredCompilation \
-XX:CompileCommand=inline,com.aws.trading.*::* \
-XX:-UseBiasedLocking \
-Djava.nio.channels.spi.SelectorProvider=sun.nio.ch.EPollSelectorProvider \
-Dsun.rmi.dgc.server.gcInterval=0x7FFFFFFFFFFFFFFE \
-Dsun.rmi.dgc.client.gcInterval=0x7FFFFFFFFFFFFFFE \
-Dfile.encoding=UTF-8 \
-server \
-jar ExchangeFlow-1.0-SNAPSHOT.jar latency-test