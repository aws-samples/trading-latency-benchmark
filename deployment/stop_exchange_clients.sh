# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

cd ./ansible
ansible-playbook stop_latency_test_on_all_clients.yaml --key-file  replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
