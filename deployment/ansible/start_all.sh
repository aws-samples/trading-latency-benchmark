export KEY_FILE="~/.ssh/id_rsa.pem"
ansible-playbook deploy_configuration.yaml --key-file $KEY_FILE -i ./inventory/exchange_client_inventory.aws_ec2.yml
ansible-playbook install_java_playbook.yaml --key-file $KEY_FILE -i ./inventory/exchange_client_inventory.aws_ec2.yml
ansible-playbook deploy_trading_mock_server.yaml --key-file $KEY_FILE -i ./inventory/exchange_client_inventory.aws_ec2.yml
ansible-playbook deploy_application.yaml --key-file $KEY_FILE -i ./inventory/exchange_client_inventory.aws_ec2.yml
ansible-playbook restart_exchange_server.yaml --key-file $KEY_FILE -i ./inventory/exchange_client_inventory.aws_ec2.yml
