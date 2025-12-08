#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

# Dynamic CPU core allocation based on tune_os.yaml configuration
# This script automatically adapts to different instance sizes
# Server cores are allocated AFTER client cores to avoid overlap

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

# Allocate cores for server based on available isolated cores
# Server gets cores AFTER the client cores to avoid overlap
# Client typically uses first 1-4 cores, so server starts after that
if [ $TOTAL_ISOLATED -ge 8 ]; then
    # Large instances: client uses 0-3, server uses 4-7 (4 cores each)
    SERVER_START=$((START_CORE + 4))
    SERVER_CORES="${SERVER_START}-$((SERVER_START + 3))"
elif [ $TOTAL_ISOLATED -ge 6 ]; then
    # Medium-large instances: client uses 0-2, server uses 3-5 (3 cores each)
    SERVER_START=$((START_CORE + 3))
    SERVER_CORES="${SERVER_START}-$((SERVER_START + 2))"
elif [ $TOTAL_ISOLATED -ge 4 ]; then
    # Medium instances: client uses 0-2, server uses 3 (1 core)
    SERVER_START=$((START_CORE + 3))
    SERVER_CORES="${SERVER_START}"
elif [ $TOTAL_ISOLATED -ge 3 ]; then
    # Small instances: client uses 0-1, server uses 2 (1 core)
    SERVER_START=$((START_CORE + 2))
    SERVER_CORES="${SERVER_START}"
elif [ $TOTAL_ISOLATED -ge 2 ]; then
    # Tiny instances: client uses 0, server uses 1 (1 core each)
    SERVER_START=$((START_CORE + 1))
    SERVER_CORES="${SERVER_START}"
else
    # Minimal instances: both share the only isolated core
    SERVER_CORES="${START_CORE}"
    echo "WARNING: Only 1 isolated core available - client and server will share"
fi

echo "Server CPU cores: $SERVER_CORES"
echo "Total isolated cores available: $TOTAL_ISOLATED"
echo "Note: Client uses cores starting from $START_CORE, server uses $SERVER_CORES"

# Set thread priorities
cd /home/ec2-user/mock-trading-server
ulimit -l unlimited

# Check if we have real-time scheduling permissions
if chrt -f 80 true 2>/dev/null; then
    echo "Using real-time scheduling (priority 80)"
    # Use localalloc for consistency with client
    numactl --localalloc -- taskset -c $SERVER_CORES chrt -f 80 ./target/release/mock-trading-server
else
    echo "WARNING: Real-time scheduling not available (need to log out/in after tune_os.yaml)"
    echo "Running without RT priority - performance may be reduced"
    echo "To fix run as sudo su - $USER"
    # Run without real-time priority
    numactl --localalloc -- taskset -c $SERVER_CORES ./target/release/mock-trading-server
fi
