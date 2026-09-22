#!/bin/bash
# Run netdev_coal_syscall.o: netdev_coal_run.sh <ifname> <set|request|get> [usecs frames]
# Prints: ret got_usecs got_frames
set -e
DIR=$(dirname "$0")
BPFTOOL=${BPFTOOL:-/home/cc/research/linux/tools/bpf/bpftool/bpftool}
OBJ=${OBJ:-$DIR/netdev_coal_syscall.o}
PIN=/sys/fs/bpf/netdev_coal
IF=$1; MODE=$2; U=${3:-0}; F=${4:-0}
case $MODE in set) M=0;; request) M=1;; get) M=2;; *) echo "bad mode" >&2; exit 2;; esac
IDX=$(cat /sys/class/net/$IF/ifindex)
[ -e $PIN ] || $BPFTOOL prog load "$OBJ" $PIN
python3 -c "import struct,sys; sys.stdout.buffer.write(struct.pack('<IIIIiII',$IDX,$U,$F,$M,0,0,0))" > $DIR/ctx_in.bin
$BPFTOOL prog run pinned $PIN ctx_in $DIR/ctx_in.bin ctx_out $DIR/ctx_out.bin >/dev/null
python3 -c "import struct; v=struct.unpack('<IIIIiII',open('$DIR/ctx_out.bin','rb').read()[:28]); print('ret=%d got=%u/%u' % (v[4],v[5],v[6]))"
