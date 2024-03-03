# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
cd ./ansible
INVENTORY="./inventory/intel_virginia_inventory.aws_ec2.yml"
SSH_KEY_FILE="~/.ssh/virginia.pem"

PLAYBOOKS=(
  "./provision_ec2.yaml"
  "./deploy_intel_qat_enabled_test_stack.yaml"
)

for playbook in "${PLAYBOOKS[@]}"; do
  ansible-playbook "$playbook" \
    --key-file "$SSH_KEY_FILE" \
    -i "$INVENTORY"
done

INVENTORY="./inventory/amd_virginia_inventory.aws_ec2.yml"
SSH_KEY_FILE="~/.ssh/virginia.pem"

PLAYBOOKS=(
  "./provision_ec2.yaml"
  "./deploy_amd_test_stack.yaml"
)

for playbook in "${PLAYBOOKS[@]}"; do
  ansible-playbook "$playbook" \
    --key-file "$SSH_KEY_FILE" \
    -i "$INVENTORY"
done
