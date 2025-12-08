#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

# Dynamic CPU core allocation based on tune_os.yaml configuration
# This script automatically adapts to different instance sizes

# Function to calculate isolated cores if config file doesn't exist
calculate_isolated_cores() {
    # Get the current online CPU count (this is the actual available cores)
    local online_cpus=$(nproc)
    
    # The online CPU count is already the effective cores because:
    # - On x86 WITH SMT: tune_os.yaml already disabled sibling threads, so nproc shows physical cores
    # - On x86 WITHOUT SMT (C7a): nproc shows physical cores (1 thread per core)
    # - On ARM/Graviton: nproc shows physical cores (no SMT)
    local effective_cores=$online_cpus
    
    local housekeeping_count
    
    # Apply same logic as tune_os.yaml based on effective cores
    if [ $effective_cores -le 4 ]; then
        housekeeping_count=1
    elif [ $effective_cores -le 8 ]; then
        housekeeping_count=2
    elif [ $effective_cores -le 32 ]; then
        housekeeping_count=4
    elif [ $effective_cores -le 96 ]; then
        housekeeping_count=8
    else
        housekeeping_count=16
    fi
    
    echo "${housekeeping_count}-$((effective_cores - 1))"
}

# Read isolated cores from tune_os configuration or calculate dynamically
if [ -f /etc/tuned/cpu-partitioning-variables.conf ]; then
    ISOLATED_CORES=$(grep '^isolated_cores=' /etc/tuned/cpu-partitioning-variables.conf | cut -d'=' -f2 | tr -d ' ')
    echo "Using isolated cores from tune_os config: $ISOLATED_CORES"
else
    ISOLATED_CORES=$(calculate_isolated_cores)
    echo "Calculated isolated cores dynamically: $ISOLATED_CORES"
fi

# Extract start and end of isolated range
START_CORE=$(echo $ISOLATED_CORES | cut -d'-' -f1)
END_CORE=$(echo $ISOLATED_CORES | cut -d'-' -f2)
TOTAL_ISOLATED=$((END_CORE - START_CORE + 1))

# Allocate cores for client based on available isolated cores
# Client gets the first portion of isolated cores
if [ $TOTAL_ISOLATED -ge 8 ]; then
    # Large instances: use 4 cores for client
    CLIENT_CORES="${START_CORE}-$((START_CORE + 3))"
    CLIENT_GC_THREADS=2
elif [ $TOTAL_ISOLATED -ge 4 ]; then
    # Medium instances: use 3 cores for client
    CLIENT_CORES="${START_CORE}-$((START_CORE + 2))"
    CLIENT_GC_THREADS=2
elif [ $TOTAL_ISOLATED -ge 2 ]; then
    # Small instances: use 2 cores for client
    CLIENT_CORES="${START_CORE}-$((START_CORE + 1))"
    CLIENT_GC_THREADS=1
else
    # Tiny instances: use 1 core for client
    CLIENT_CORES="${START_CORE}"
    CLIENT_GC_THREADS=1
fi

echo "Client CPU cores: $CLIENT_CORES (GC threads: $CLIENT_GC_THREADS)"
echo "Total isolated cores available: $TOTAL_ISOLATED"

# Check if we have real-time scheduling permissions
if chrt -f 80 true 2>/dev/null; then
    echo "Using real-time scheduling (priority 80)"
    RT_PRIORITY="chrt -f 80"
else
    echo "WARNING: Real-time scheduling not available (need to log out/in after tune_os.yaml)"
    echo "Running without RT priority - performance may be reduced"
    echo "To fix run as sudo su - $USER"
    RT_PRIORITY=""
fi

# Dynamic memory configuration based on available memory and tune_os.yaml settings
TOTAL_MEM_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
TOTAL_MEM_GB=$((TOTAL_MEM_KB / 1024 / 1024))

# Calculate heap size: use 25% of total memory, minimum 1GB, maximum 8GB for trading app
if [ $TOTAL_MEM_GB -le 4 ]; then
    HEAP_SIZE="1g"
elif [ $TOTAL_MEM_GB -le 16 ]; then
    HEAP_SIZE="2g"
elif [ $TOTAL_MEM_GB -le 32 ]; then
    HEAP_SIZE="4g"
else
    HEAP_SIZE="8g"
fi

# Calculate direct memory (for Netty buffers): 50% of heap
HEAP_SIZE_NUM=$(echo $HEAP_SIZE | sed 's/g//')
DIRECT_MEM=$((HEAP_SIZE_NUM * 512))  # In MB

echo "JVM Heap Size: $HEAP_SIZE (Direct Memory: ${DIRECT_MEM}m)"

# Check if hugepages are configured by tune_os.yaml
HUGEPAGES_AVAIL=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
HUGEPAGE_SIZE=$(grep Hugepagesize /proc/meminfo | awk '{print $2}')

if [ "$HUGEPAGES_AVAIL" -gt 0 ] 2>/dev/null; then
    echo "Hugepages detected: $HUGEPAGES_AVAIL pages of ${HUGEPAGE_SIZE}KB each"
    HUGEPAGE_FLAGS="-XX:+UseLargePages -XX:LargePageSizeInBytes=${HUGEPAGE_SIZE}k"
else
    echo "Hugepages not configured - using regular pages"
    HUGEPAGE_FLAGS=""
fi

# Adjust Netty arenas based on number of cores allocated
if [ $TOTAL_ISOLATED -ge 8 ]; then
    NETTY_ARENAS=4
elif [ $TOTAL_ISOLATED -ge 4 ]; then
    NETTY_ARENAS=3
else
    NETTY_ARENAS=2
fi

echo "Netty Direct Arenas: $NETTY_ARENAS"

# -Dio.netty.handler.ssl.openssl.engine=qatengine
# Use --localalloc instead of specific node binding
numactl --localalloc -- taskset -c $CLIENT_CORES $RT_PRIORITY java \
-Xms${HEAP_SIZE} -Xmx${HEAP_SIZE} \
-XX:MaxDirectMemorySize=${DIRECT_MEM}m \
-XX:+AlwaysPreTouch \
$HUGEPAGE_FLAGS \
-XX:+UnlockExperimentalVMOptions \
-XX:+UseZGC \
-XX:ConcGCThreads=$CLIENT_GC_THREADS \
-XX:ZCollectionInterval=300 \
-XX:+UseNUMA \
-XX:+UnlockDiagnosticVMOptions \
-XX:GuaranteedSafepointInterval=0 \
-XX:+UseCountedLoopSafepoints \
-XX:+DisableExplicitGC \
-XX:+UseCompressedOops \
-XX:+UseTLAB \
-XX:+UseThreadPriorities \
-XX:ThreadPriorityPolicy=1 \
-XX:CompileThreshold=1000 \
-XX:+TieredCompilation \
-XX:CompileCommand=inline,com.aws.trading.*::* \
-Djava.nio.channels.spi.SelectorProvider=sun.nio.ch.EPollSelectorProvider \
-Dsun.rmi.dgc.server.gcInterval=0x7FFFFFFFFFFFFFFE \
-Dsun.rmi.dgc.client.gcInterval=0x7FFFFFFFFFFFFFFE \
-Dfile.encoding=UTF-8 \
-Dio.netty.allocator.numDirectArenas=$NETTY_ARENAS \
-Dio.netty.allocator.numHeapArenas=0 \
-Dio.netty.allocator.tinyCacheSize=256 \
-Dio.netty.allocator.smallCacheSize=64 \
-Dio.netty.allocator.normalCacheSize=32 \
-Dio.netty.allocator.maxOrder=9 \
-Dio.netty.buffer.checkBounds=false \
-Dio.netty.buffer.checkAccessible=false \
-Dio.netty.leakDetection.level=DISABLED \
-Dio.netty.recycler.maxCapacity=32 \
-Dio.netty.eventLoop.maxPendingTasks=1024 \
-Dio.netty.noPreferDirect=false \
-server \
-jar ExchangeFlow-1.0-SNAPSHOT.jar latency-test
