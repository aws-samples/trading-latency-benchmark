# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

echo "1. Removing previous histogram logs"
rm -rf ../../histogram_logs/*
echo "2. Removing previous histogram logs history"
rm -rf ../../histogram_logs_history/*
echo "3. Stopping all test clients"
ansible-playbook stop_latency_test_on_all_clients.yaml --key-file  replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
echo "3. Restarting and cleaning up exchange servers"
ansible-playbook restart_exchange_server.yaml --key-file  replace_me_with_ssh_key_pair -i ./inventory/exchange_server_inventory.aws_ec2.yml
echo "3. Cleaning up test clients"
ansible-playbook cleanup_all_clients.yaml --key-file  replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
