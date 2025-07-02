#!/bin/bash

#
# Performance Benchmark Script for AF_XDP Zero-Copy vs Standard Sockets
# This script runs Binance trade feed round-trip latency benchmarks
#

# Configuration
INTERFACE="enp39s0"
REPLICATOR_IP="10.0.0.71"
REPLICATOR_PORT="9000"
LOCAL_IP="10.0.0.34"
LOCAL_PORT="9001"
TOTAL_MESSAGES=1000000  # 1 million messages per test
MESSAGE_RATES=(1000 5000 10000 50000 100000)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if running as root (needed for AF_XDP)
check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo -e "${RED}This script must be run as root for AF_XDP operations${NC}"
        exit 1
    fi
}

# Print header
print_header() {
    echo -e "${BLUE}============================================${NC}"
    echo -e "${BLUE}AF_XDP Zero-Copy Round-Trip Benchmark${NC}"
    echo -e "${BLUE}============================================${NC}"
    echo "Configuration:"
    echo "  Interface: $INTERFACE"
    echo "  Replicator: $REPLICATOR_IP:$REPLICATOR_PORT"
    echo "  Local Endpoint: $LOCAL_IP:$LOCAL_PORT"
    echo "  Total Messages: $TOTAL_MESSAGES"
    echo "  Message Rates: ${MESSAGE_RATES[@]} msg/sec"
    echo -e "${BLUE}============================================${NC}\n"
}

# Clean up existing XDP programs
cleanup_xdp() {
    echo -e "${YELLOW}Cleaning up existing XDP programs...${NC}"
    ./cleanup.sh 2>/dev/null || true
    sleep 2
}

# Start packet replicator
start_replicator() {
    local mode=$1
    local log_file=$2
    
    echo -e "${GREEN}Starting packet replicator in $mode mode...${NC}"
    
    if [ "$mode" == "zero-copy" ]; then
        # AF_XDP zero-copy mode (default)
        ./packet_replicator $INTERFACE $REPLICATOR_IP $REPLICATOR_PORT > "$log_file" 2>&1 &
    else
        # Standard socket mode (force non-zero-copy)
        ./packet_replicator $INTERFACE $REPLICATOR_IP $REPLICATOR_PORT false > "$log_file" 2>&1 &
    fi
    
    local pid=$!
    sleep 5  # Wait for replicator to start
    
    # Check if replicator started successfully
    if ! kill -0 $pid 2>/dev/null; then
        echo -e "${RED}Failed to start packet replicator${NC}"
        cat "$log_file"
        exit 1
    fi
    
    echo $pid
}

# Add subscriber
add_subscriber() {
    echo -e "${YELLOW}Adding subscriber $SUBSCRIBER_IP:$SUBSCRIBER_PORT...${NC}"
    ./control_client $REPLICATOR_IP add $SUBSCRIBER_IP $SUBSCRIBER_PORT
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}Failed to add subscriber${NC}"
        return 1
    fi
    
    sleep 2
    return 0
}

# Run round-trip benchmark for a specific rate
run_benchmark() {
    local mode=$1
    local rate=$2
    local result_file=$3
    local log_file=$4
    
    echo -e "${GREEN}Running round-trip benchmark: $mode mode, $rate msg/sec${NC}"
    
    # Run market data provider client with round-trip measurement
    # Parameters: replicator_ip port local_ip local_port total_messages rate
    ./market_data_provider_client $REPLICATOR_IP $REPLICATOR_PORT $LOCAL_IP $LOCAL_PORT $TOTAL_MESSAGES $rate > "$result_file" 2>&1
    
    # Extract statistics from output
    local messages_sent=$(grep "Total messages sent:" "$result_file" | awk '{print $4}')
    local messages_received=$(grep "Total messages received:" "$result_file" | awk '{print $4}')
    local packet_loss=$(grep "Packet loss:" "$result_file" | awk -F'[()]' '{print $2}')
    local actual_rate=$(grep "Actual send rate:" "$result_file" | awk '{print $4}')
    
    # Extract RTT statistics
    local min_rtt=$(grep "Min RTT:" "$result_file" | awk '{print $3}')
    local avg_rtt=$(grep "Avg RTT:" "$result_file" | awk '{print $3}')
    local max_rtt=$(grep "Max RTT:" "$result_file" | awk '{print $3}')
    
    # Extract percentiles
    local p50=$(grep "50%:" "$result_file" | awk '{print $2}')
    local p90=$(grep "90%:" "$result_file" | awk '{print $2}')
    local p95=$(grep "95%:" "$result_file" | awk '{print $2}')
    local p99=$(grep "99%:" "$result_file" | awk '{print $2}')
    local p999=$(grep "99.9%:" "$result_file" | awk '{print $2}')
    
    echo "  Messages: $messages_sent sent, $messages_received received"
    echo "  Packet Loss: $packet_loss"
    echo "  Actual Rate: $actual_rate msg/sec"
    echo "  Min RTT: $min_rtt μs"
    echo "  Avg RTT: $avg_rtt μs"
    echo "  Max RTT: $max_rtt μs"
    echo "  P50: $p50 μs"
    echo "  P90: $p90 μs"
    echo "  P95: $p95 μs"
    echo "  P99: $p99 μs"
    echo "  P99.9: $p999 μs"
    echo ""
    
    # Save results to CSV
    echo "$mode,$rate,$actual_rate,$packet_loss,$min_rtt,$avg_rtt,$max_rtt,$p50,$p90,$p95,$p99,$p999" >> benchmark_results.csv
}

# Main benchmark function
main() {
    check_root
    print_header
    
    # Create results directory
    mkdir -p results
    timestamp=$(date +%Y%m%d_%H%M%S)
    
    # Initialize CSV file
    echo "Mode,Target_Rate,Actual_Rate,Packet_Loss,Min_RTT,Avg_RTT,Max_RTT,P50,P90,P95,P99,P99.9" > benchmark_results.csv
    
    # Test both modes
    for mode in "zero-copy" "standard"; do
        echo -e "\n${BLUE}==== Testing $mode mode ====${NC}\n"
        
        cleanup_xdp
        
        # Start replicator
        log_file="results/replicator_${mode}_${timestamp}.log"
        pid=$(start_replicator "$mode" "$log_file")
        
        # Add subscriber
        if ! add_subscriber; then
            kill $pid 2>/dev/null
            continue
        fi
        
        # Warmup
        echo -e "${YELLOW}Warming up...${NC}"
        ./market_data_provider_client $REPLICATOR_IP $REPLICATOR_PORT $LOCAL_IP $LOCAL_PORT 1000 1000 > /dev/null 2>&1
        
        # Run benchmarks at different rates
        for rate in "${MESSAGE_RATES[@]}"; do
            result_file="results/benchmark_${mode}_${rate}_${timestamp}.txt"
            run_benchmark "$mode" "$rate" "$result_file" "$log_file"
            sleep 5  # Cool down between tests
        done
        
        # Stop replicator
        echo -e "${YELLOW}Stopping packet replicator...${NC}"
        kill $pid 2>/dev/null
        wait $pid 2>/dev/null
        
        sleep 5
    done
    
    echo -e "\n${BLUE}==== Benchmark Complete ====${NC}"
    echo -e "${GREEN}Results saved to:${NC}"
    echo "  - benchmark_results.csv"
    echo "  - results/"
    
    # Generate summary report
    generate_report
}

# Generate summary report
generate_report() {
    echo -e "\n${BLUE}==== Performance Comparison Report ====${NC}\n"
    
    # Create a simple comparison table
    echo "Message Rate Comparison (Average Latency in microseconds):"
    echo "-------------------------------------------------------------"
    printf "%-15s" "Rate (msg/s)"
    printf "%-20s" "AF_XDP Zero-Copy"
    printf "%-20s" "Standard Socket"
    printf "%-15s\n" "Improvement"
    echo "-------------------------------------------------------------"
    
    for rate in "${MESSAGE_RATES[@]}"; do
        zero_copy_avg=$(grep "zero-copy,$rate," benchmark_results.csv | cut -d',' -f5)
        standard_avg=$(grep "standard,$rate," benchmark_results.csv | cut -d',' -f5)
        
        if [ -n "$zero_copy_avg" ] && [ -n "$standard_avg" ]; then
            improvement=$(echo "scale=2; (($standard_avg - $zero_copy_avg) / $standard_avg) * 100" | bc)
            printf "%-15s" "$rate"
            printf "%-20s" "$zero_copy_avg μs"
            printf "%-20s" "$standard_avg μs"
            printf "%-15s\n" "$improvement%"
        fi
    done
    
    echo "-------------------------------------------------------------"
    
    # Save full report
    {
        echo "AF_XDP Zero-Copy Performance Benchmark Report"
        echo "Generated: $(date)"
        echo ""
        cat benchmark_results.csv
    } > "results/benchmark_report_$(date +%Y%m%d_%H%M%S).txt"
}

# Signal handlers
trap 'echo -e "\n${RED}Benchmark interrupted${NC}"; cleanup; exit 1' INT TERM

# Cleanup function
cleanup() {
    echo -e "${YELLOW}Cleaning up...${NC}"
    pkill -f packet_replicator 2>/dev/null || true
    ./cleanup.sh 2>/dev/null || true
}

# Run the benchmark
main "$@"
