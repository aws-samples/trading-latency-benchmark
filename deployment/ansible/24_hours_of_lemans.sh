# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

echo "1. Stopping all clients"
ansible-playbook stop_latency_test_on_all_clients.yaml --key-file replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
echo "2. Stopping all servers and cleaning up servers, and Restarting them"
ansible-playbook restart_exchange_server.yaml --key-file  replace_me_with_ssh_key_pair -i ./inventory/exchange_server_inventory.aws_ec2.yml
echo "3. Fetching histogram logs here"
ansible-playbook fetch_histogram_logs.yaml --key-file replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
cp -r ../../histogram_logs ../../histogram_logs_history/`stat --printf=%Y ../../histogram_logs | sed -e 's/ .*//'`
echo "4. Starting latency test"
ansible-playbook start_latency_test_on_all_clients.yaml --key-file replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
