# 
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy of this
# software and associated documentation files (the "Software"), to deal in the Software
# without restriction, including without limitation the rights to use, copy, modify,
# merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
# PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# 
# Disable hyperthreading
for cpunum in $(cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list | cut -s -d, -f2- | tr ',' '\n' | sort -un) do echo 0 > /sys/devices/system/cpu/cpu$cpunum/online
done

#Kernel workqueues (move all work queues to core 0 and 1)
find /sys/devices/virtual/workqueue -name cpumask -exec sh -c 'echo 3 > {}' ';'

#P-state (set it to maximum)
echo "performance" | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

#IO scheduler
echo "mq-deadline" | tee /sys/block/*/queue/schedulerecho 1 | tee /sys/block/*/queue/iosched/fifo_batch

#Clock source (Time Stamp Counter)
echo tsc | tee /sys/devices/system/clocksource/clocksource0/current_clocksource

#NUMA affinity for writeback threads, move threads should be moved to only the housekeeping CPUs
echo 0 | tee /sys/bus/workqueue/devices/writeback/numa

#Sysctl IO configuration
sysctl -w vm.dirty_background_bytes=33554432
sysctl -w vm.dirty_bytes=268435456
sysctl -w vm.dirty_expire_centisecs=3000
sysctl -w vm.dirty_writeback_centisecs=10
sysctl -w vm.swappiness=0

#Sysctl network
sysctl -w net.core.rmem_max=2097152
sysctl -w net.core.wmem_max=2097152
sysctl -w net.core.busy_poll=1
sysctl -w net.core.default_qdisc=noqueue

#Sysctl kernel
sysctl -w kernel.watchdog=0
sysctl -w kernel.perf_event_paranoid=-1
sysctl -w kernel.numa_balancing=0

#Disable irqbalance
systemctl stop irqbalance; systemctl disable irqbalance

#Disable AWS SSM agent
systemctl stop amazon-ssm-agent; systemctl disable amazon-ssm-agent

#Disable iptables
modprobe -rv ip_tables

#ENA driver settings
ethtool -K eth0 rx off tx off sg off gso off gro off

#Enable static interrupt moderation
ethtool -C eth0 adaptive-rx off rx-usecs 1 tx-usecs 1

#Disable syscall auditing (leave auditd functioning)
echo "-a never,task" > /etc/audit/rules.d/disable-syscall-auditing.rules
/sbin/augenrules --load

#Receive Side Scaling
export IRQS=($(grep eth0 /proc/interrupts | awk '{print $1}' | tr -d :))for i in ${!IRQS[@]}; do echo $i > /proc/irq/${IRQS[i]}/smp_affinity_list; done;

Transmit Packet Steering
export TXQUEUES=($(ls -1qdv /sys/class/net/eth0/queues/tx-*))for i in ${!TXQUEUES[@]}; do printf '%x' $((2**i)) > ${TXQUEUES[i]}/xps_cpus; done;

DHCP Client
dhclient -x -pf /var/run/dhclient-eth0.pid
ip addr change $( ip -4 addr show dev eth0 | grep 'inet' | awk '{ print $2 " brd " $4 " scope global"}') dev eth0 valid_lft forever preferred_lft forever

Kernel boot options
transparent_hugepage=never audit=0 nmi_watchdog=0 nohz=on clocksource=tsc nosoftlockup mce=ignore_ce cpuidle.off=1 skew_tick=1acpi_irq_nobalance intel_pstate=disable intel_idle.max_cstate=0 processor.max_cstate=0 idle=poll isolcpus=2-15 nohz_full=2-15
