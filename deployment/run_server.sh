#!/bin/bash
# Set thread priorities
cd /home/ec2-user/mock-trading-server
ulimit -l unlimited

# Use localalloc for consistency with client
numactl --localalloc -- taskset -c 5-7 chrt -f 80 ./target/release/mock-trading-server