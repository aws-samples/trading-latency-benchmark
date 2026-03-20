#!/bin/bash
# Re-establish SSH tunnels to demo instances
# Usage: ./tunnels.sh [start|stop|status]
#
# Edit the variables below to match your deployment:

KEY=~/.ssh/<your-key>.pem
PUB=<publisher-public-ip>
SUB1=<subscriber1-public-ip>
SUB2=<subscriber2-public-ip>
SSH_OPTS="-o StrictHostKeyChecking=no -o ServerAliveInterval=15 -o ServerAliveCountMax=20"

case "${1:-start}" in
  stop)
    echo "Killing tunnels..."
    pkill -f "ssh.*-L 888[123]" 2>/dev/null
    echo "Done."
    ;;

  status)
    echo -n "Publisher (8881): "
    curl -s --max-time 2 http://localhost:8881/api/status || echo "DOWN"
    echo
    echo -n "Sub1     (8882): "
    curl -s --max-time 2 http://localhost:8882/api/status || echo "DOWN"
    echo
    echo -n "Sub2     (8883): "
    curl -s --max-time 2 http://localhost:8883/api/status || echo "DOWN"
    echo
    ;;

  start|*)
    # Kill existing
    pkill -f "ssh.*-L 888[123]" 2>/dev/null
    sleep 1

    echo "Opening tunnels..."
    ssh -i $KEY $SSH_OPTS -L 8881:localhost:8888 ec2-user@$PUB -N &
    ssh -i $KEY $SSH_OPTS -L 8882:localhost:8888 ec2-user@$SUB1 -N &
    ssh -i $KEY $SSH_OPTS -L 8883:localhost:8888 ec2-user@$SUB2 -N &
    sleep 3

    echo ""
    echo -n "Publisher (8881): "
    curl -s --max-time 2 http://localhost:8881/api/status || echo "DOWN"
    echo
    echo -n "Sub1     (8882): "
    curl -s --max-time 2 http://localhost:8882/api/status || echo "DOWN"
    echo
    echo -n "Sub2     (8883): "
    curl -s --max-time 2 http://localhost:8883/api/status || echo "DOWN"
    echo
    echo ""
    echo "Dashboard: file://$(cd "$(dirname "$0")" && pwd)/dashboard.html?pub=localhost:8881&sub1=localhost:8882&sub2=localhost:8883"
    ;;
esac
