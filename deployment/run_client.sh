#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

# -Dio.netty.handler.ssl.openssl.engine=qatengine
# Use --localalloc instead of specific node binding
numactl --localalloc -- taskset -c 2-4 chrt -f 80 java \
-Xms7g -Xmx7g \
-XX:+AlwaysPreTouch \
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
-Dio.netty.allocator.numDirectArenas=3 \
-Dio.netty.allocator.numHeapArenas=0 \
-Dio.netty.allocator.tinyCacheSize=256 \
-Dio.netty.allocator.smallCacheSize=64 \
-Dio.netty.allocator.normalCacheSize=32 \
-Dio.netty.buffer.checkBounds=false \
-Dio.netty.buffer.checkAccessible=false \
-Dio.netty.leakDetection.level=DISABLED \
-Dio.netty.recycler.maxCapacity=32 \
-Dio.netty.eventLoop.maxPendingTasks=1024 \
-server \
-jar ExchangeFlow-1.0-SNAPSHOT.jar latency-test