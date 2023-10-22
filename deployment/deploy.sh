# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

cd ../
mvn clean install
cd ./deployment/ansible
ansible-playbook deploy_application.yaml --key-file  replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
ansible-playbook deploy_configuration.yaml --key-file replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml

