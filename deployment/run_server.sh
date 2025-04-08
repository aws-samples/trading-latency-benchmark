#!/bin/bash
# Set thread priorities
cd /home/ec2-user/mock-trading-server
ulimit -l unlimited

# Start server with NUMA binding and core affinity
# Equal priority to client to prevent priority inversion
exec numactl --membind=0 taskset -c 4-9 chrt -f 80 ./target/release/mock-trading-server