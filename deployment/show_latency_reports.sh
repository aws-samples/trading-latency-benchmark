# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

cd ansible
ansible-playbook fetch_histogram_logs.yaml --key-file  replace_me_with_ssh_key_pair -i ./inventory/exchange_client_inventory.aws_ec2.yml
cd ../

function command() {
  echo $1
  java -jar ../target/ExchangeFlow-1.0-SNAPSHOT.jar latency-report $1
}

for f in $(find ../histogram_logs -name '*.hlog'); do command $f; done
