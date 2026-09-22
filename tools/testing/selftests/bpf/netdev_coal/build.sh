#!/bin/bash
# Build the BPF objects used by the scripts in this directory (outputs land here).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../../../../.." && pwd)
P=$ROOT/tools/testing/selftests/bpf/progs
CFLAGS="-g -O2 -target bpf -D__TARGET_ARCH_x86 -mcpu=v3 -I $ROOT/tools/testing/selftests/bpf/include -I $ROOT/tools/include/uapi"
clang $CFLAGS -c $P/netdev_coal_syscall.c -o $DIR/netdev_coal_syscall.o
clang $CFLAGS -c $P/cpufreq_ext_coal.c -o $DIR/cpufreq_ext_coal.o
clang $CFLAGS -DCOAL_SYNC -c $P/cpufreq_ext_coal.c -o $DIR/cpufreq_ext_coal_sync.o
clang $CFLAGS -c $P/cpuidle_ext_coal.c -o $DIR/cpuidle_ext_coal.o
echo "built: $(cd $DIR && ls *.o | tr '\n' ' ')"
