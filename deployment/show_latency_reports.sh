#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

# Configuration variables
INVENTORY="./ansible/inventory/inventory.aws_ec2.yml"
SSH_KEY_FILE="${SSH_KEY_FILE:-$HOME/.ssh/virginia_keypair.pem}"
OUTPUT_DIR="../histogram_logs"
REPORT_SUMMARY_FILE="latency_report_summary.txt"

# Function to display usage information
function show_usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -i, --inventory INVENTORY_FILE  Specify the Ansible inventory file"
    echo "  -k, --key SSH_KEY_FILE          Specify the SSH key file"
    echo "  -o, --output OUTPUT_DIR         Specify the output directory for logs"
    echo "  -h, --help                      Show this help message"
    echo ""
    echo "Example:"
    echo "  $0 --inventory ./ansible/inventory/custom_inventory.aws_ec2.yml --key ~/.ssh/my_key.pem"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        -i|--inventory)
            INVENTORY="$2"
            shift 2
            ;;
        -k|--key)
            SSH_KEY_FILE="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Validate required parameters
if [ ! -f "$INVENTORY" ]; then
    echo "Error: Inventory file not found: $INVENTORY"
    echo "Please specify a valid inventory file with --inventory option"
    exit 1
fi

if [ ! -f "$(eval echo $SSH_KEY_FILE)" ]; then
    echo "Error: SSH key file not found: $SSH_KEY_FILE"
    echo "Please specify a valid SSH key file with --key option"
    exit 1
fi

# Ensure output directory exists
mkdir -p "$OUTPUT_DIR"

echo "========================================================"
echo "Trading Latency Benchmark Report Generator"
echo "========================================================"
echo "Inventory file: $INVENTORY"
echo "SSH key file: $SSH_KEY_FILE"
echo "Output directory: $OUTPUT_DIR"
echo "========================================================"

# Run Ansible playbook to fetch logs
echo "Fetching histogram logs from EC2 instances..."
cd ansible
ansible-playbook fetch_histogram_logs.yaml --key-file "$SSH_KEY_FILE" -i "$INVENTORY"
ANSIBLE_EXIT_CODE=$?
cd ..

if [ $ANSIBLE_EXIT_CODE -ne 0 ]; then
    echo "Error: Failed to fetch histogram logs from EC2 instances"
    exit 1
fi

echo "Logs fetched successfully"
echo "========================================================"

# Check if the Java jar file exists, build if needed
JAR_FILE="../hft_client/java_client/target/ExchangeFlow-1.0-SNAPSHOT.jar"
if [ ! -f "$JAR_FILE" ]; then
    echo "Jar file not found at $JAR_FILE, building with Maven..."
    (cd ../hft_client/java_client && mvn clean install -q -DskipTests)
    if [ $? -ne 0 ]; then
        echo "Error: Maven build failed"
        exit 1
    fi
    echo "Build successful"
fi

echo "Generating latency reports..."

# Initialize summary file
echo "# Latency Report Summary - $(date)" > "$OUTPUT_DIR/$REPORT_SUMMARY_FILE"
echo "| Instance | Min (μs) | p50 (μs) | p90 (μs) | p99 (μs) | p99.9 (μs) | Max (μs) |" >> "$OUTPUT_DIR/$REPORT_SUMMARY_FILE"
echo "|----------|----------|----------|----------|----------|------------|----------|" >> "$OUTPUT_DIR/$REPORT_SUMMARY_FILE"

# Process each histogram log file
FOUND_FILES=0
for log_file in $(find "$OUTPUT_DIR" -name "*.hlog" -type f); do
    FOUND_FILES=1
    echo "Processing: $log_file"
    
    # Extract instance name from parent directory (e.g. Latency-Hunting-Probe-arm-c8g-9)
    instance_name=$(basename "$(dirname "$log_file")")
    # If the file is directly in OUTPUT_DIR (no subdirectory), fall back to filename
    if [ "$instance_name" = "$(basename "$OUTPUT_DIR")" ]; then
        instance_name=$(basename "$log_file" .hlog)
    fi
    
    # Run the Java program to analyze the log
    echo "Running analysis for $instance_name..."
    REPORT_OUTPUT=$(java -jar "$JAR_FILE" latency-report "$log_file")
    
    if [ $? -ne 0 ]; then
        echo "Error: Failed to generate report for $log_file"
        continue
    fi
    
    # Display the report
    echo "$REPORT_OUTPUT"
    
    # Extract key metrics for summary
    min=$(echo "$REPORT_OUTPUT" | grep "Min latency:" | awk '{print $3}')
    p50=$(echo "$REPORT_OUTPUT" | grep "50.00%:" | awk '{print $2}')
    p90=$(echo "$REPORT_OUTPUT" | grep "90.00%:" | awk '{print $2}')
    p99=$(echo "$REPORT_OUTPUT" | grep "99.00%:" | awk '{print $2}')
    p999=$(echo "$REPORT_OUTPUT" | grep "99.90%:" | awk '{print $2}')
    max=$(echo "$REPORT_OUTPUT" | grep "Max latency:" | awk '{print $3}')
    
    # Add to summary file
    echo "| $instance_name | $min | $p50 | $p90 | $p99 | $p999 | $max |" >> "$OUTPUT_DIR/$REPORT_SUMMARY_FILE"
    
    # Save detailed report to file
    echo "$REPORT_OUTPUT" > "$OUTPUT_DIR/${instance_name}_report.txt"
    
    echo "========================================================"
done

if [ $FOUND_FILES -eq 0 ]; then
    echo "No histogram log files found in $OUTPUT_DIR"
    exit 1
fi

echo "Summary report saved to: $OUTPUT_DIR/$REPORT_SUMMARY_FILE"
echo "Individual reports saved to: $OUTPUT_DIR/<instance>_report.txt"
echo "========================================================"
echo "Analysis complete!"
