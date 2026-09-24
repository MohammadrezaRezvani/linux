#!/bin/bash
# Post-boot verification of net/ethtool/bpf_coalesce.c. Run as root.
# Restores the NIC's rx-usecs and adaptive-rx on exit. Prints PASS/FAIL.
DIR=$(cd "$(dirname "$0")" && pwd)
export BPFTOOL=${BPFTOOL:-/home/cc/research/linux/tools/bpf/bpftool/bpftool}
IF=${IF:-eno1np0}
T=/sys/kernel/tracing
RUN="$DIR/netdev_coal_run.sh"
FAIL=0

show() { ethtool -c $IF | awk -v k="$1" '$0 ~ "^"k":" {print $NF}'; }
orig_u=$(show rx-usecs)
orig_f=$(show rx-frames)
orig_a=$(ethtool -c $IF | awk '/^Adaptive RX:/{print $3}')

cleanup() {
	echo 0 > $T/tracing_on; echo nop > $T/current_tracer
	for e in hwrmsend workfn applyfn; do
		echo 0 > $T/events/kprobes/$e/enable 2>/dev/null
		echo "-:$e" >> $T/kprobe_events 2>/dev/null
	done
	echo > $T/trace; echo 1 > $T/tracing_on
	ethtool -C $IF adaptive-rx ${orig_a:-off} 2>/dev/null
	ethtool -C $IF rx-usecs $orig_u
	rm -f /sys/fs/bpf/netdev_coal
	echo "restored $IF rx-usecs=$(show rx-usecs) adaptive-rx=${orig_a:-off}"
	[ $FAIL = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL ($FAIL checks)"
}
trap cleanup EXIT

hdr() { echo; echo "=== $*"; }
check() { if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got '$2' want '$3'"; FAIL=$((FAIL+1)); fi; }
probes() {
	echo 0 > $T/tracing_on; echo > $T/trace; echo nop > $T/current_tracer
	echo 'p:hwrmsend bnxt_en:hwrm_req_send type=+0(%si):x16 ring=+16(%si):u16 tmr_max=+30(%si):u16 aggr_int=+32(%si):u16' >> $T/kprobe_events
	echo 'p:workfn netdev_coal_workfn' >> $T/kprobe_events
	echo 'p:applyfn netdev_apply_rx_coalesce usecs=%si:u32' >> $T/kprobe_events
	for e in hwrmsend workfn applyfn; do echo 1 > $T/events/kprobes/$e/enable; done
}
sends() { grep -c 'hwrmsend:.*type=0x53' $T/trace; }
workers() { grep -c 'workfn:' $T/trace; }
go() { echo > $T/trace; echo 1 > $T/tracing_on; "$@"; sleep ${WAIT:-0.3}; echo 0 > $T/tracing_on; }
rings=$(ls -d /sys/class/net/$IF/queues/rx-* | wc -l)

hdr "1. kfuncs in vmlinux BTF"
check "kfuncs" "$($BPFTOOL btf dump file /sys/kernel/btf/vmlinux | grep -oE "FUNC 'bpf_netdev_[a-z_]+'" | sort -u | wc -l)" 3
probes
ethtool -C $IF adaptive-rx off

hdr "2. get before BPF touched $IF"
check "get" "$($RUN $IF get)" "ret=-2 got=0"

hdr "3. set 50 (rx-frames, CQE mode and link unchanged)"
cqe=$(ethtool -c $IF | grep 'CQE mode')
go $RUN $IF set 50 >/dev/null
check "rx-usecs" "$(show rx-usecs)" 50
check "rx-frames" "$(show rx-frames)" "$orig_f"
check "CQE mode" "$(ethtool -c $IF | grep 'CQE mode')" "$cqe"
check "link" "$(cat /sys/class/net/$IF/operstate)" up
check "fw 0x53 sends" "$(sends)" "$rings"
check "get" "$($RUN $IF get)" "ret=0 got=50"

hdr "4. set to the current value: no driver call (like ethtool)"
go $RUN $IF set 50 >/dev/null
check "fw 0x53 sends" "$(sends)" 0

hdr "5. request 100 (worker applies it)"
go $RUN $IF request 100 >/dev/null
check "rx-usecs" "$(show rx-usecs)" 100
check "worker runs" "$(workers)" 1
check "get" "$($RUN $IF get)" "ret=0 got=100"

hdr "6. duplicate request 100 x50 (worker must not run)"
go bash -c "for i in \$(seq 50); do $RUN $IF request 100 >/dev/null; done"
check "worker runs" "$(workers)" 0

hdr "7. request 0 (accepted like ethtool -C rx-usecs 0)"
go $RUN $IF request 0 >/dev/null
check "rx-usecs" "$(show rx-usecs)" 0

hdr "8. ethtool -C changes $IF behind BPF's back"
ethtool -C $IF rx-usecs 70
check "get follows ethtool" "$($RUN $IF get)" "ret=0 got=70"
ethtool -C $IF rx-usecs 30
go $RUN $IF request 70 >/dev/null
check "request 70 after ethtool 70->30" "$(show rx-usecs)" 70
go $RUN $IF request 70 >/dev/null
check "then duplicate: worker runs" "$(workers)" 0

hdr "9. burst: request 8 then 120 (last value wins)"
WAIT=0.5 go bash -c "$RUN $IF request 8 >/dev/null; $RUN $IF request 120 >/dev/null"
check "rx-usecs" "$(show rx-usecs)" 120
check "get" "$($RUN $IF get)" "ret=0 got=120"

hdr "10. error cases"
check "bogus ifindex" "$($RUN 99999 set 50)" "ret=-19 got=0"
ethtool -C $IF adaptive-rx on
check "adaptive-rx on: set" "$($RUN $IF set 60)" "ret=-16 got=0"
ethtool -C $IF adaptive-rx off
