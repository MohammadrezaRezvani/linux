#!/bin/bash
# Run netdev_coal_syscall.o: netdev_coal_run.sh <ifname|ifindex> <set|request|get> [usecs]
# Prints: ret=<ret> got=<usecs>
set -e
DIR=$(dirname "$0")
BPFTOOL=${BPFTOOL:-/home/cc/research/linux/tools/bpf/bpftool/bpftool}
OBJ=${OBJ:-$DIR/netdev_coal_syscall.o}
PIN=/sys/fs/bpf/netdev_coal
IF=$1; MODE=$2; U=${3:-0}
case $MODE in set) M=0;; request) M=1;; get) M=2;; *) echo "bad mode" >&2; exit 2;; esac
if [[ $IF =~ ^[0-9]+$ ]]; then IDX=$IF; else IDX=$(cat /sys/class/net/$IF/ifindex); fi
[ -e $PIN ] || $BPFTOOL prog load "$OBJ" $PIN
python3 $DIR/netdev_coal_run.py $PIN $IDX $M $U
