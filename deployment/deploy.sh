# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

cd ../
mvn clean install
cd ./deployment/ansible

INVENTORY="./inventory/virginia_inventory.aws_ec2.yml"
SSH_KEY_FILE="~/.ssh/virginia.pem"

PLAYBOOKS=(
  "install_java_playbook.yaml"
  "deploy_hft_client_application.yaml"
  "deploy_hft_client_configuration.yaml"
  "deploy_mock_trading_server.yaml"
)

for playbook in "${PLAYBOOKS[@]}"; do
  ansible-playbook "$playbook" \
    --key-file "$SSH_KEY_FILE" \
    -i "$INVENTORY"
done
