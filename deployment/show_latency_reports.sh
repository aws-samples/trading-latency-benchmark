# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
INVENTORY="./inventory/virginia_inventory.aws_ec2.yml"
SSH_KEY_FILE="~/.ssh/virginia_keypair.pem"

cd ansible
ansible-playbook fetch_histogram_logs.yaml --key-file $SSH_KEY_FILE -i $INVENTORY
cd ../

function command() {
  echo $1
  java -jar ../target/ExchangeFlow-1.0-SNAPSHOT.jar latency-report $1
}

for f in $(find ../histogram_logs -name '*.hlog'); do command $f; done
