#!/bin/bash
# Check that bpf_netdev_set_rx_coalesce(U) and bpf_netdev_request_rx_coalesce(U)
# do exactly what "ethtool -C IF rx-usecs U" does. Run as root.
# For each U, starting from the same baseline every time, this compares:
#   - the full "ethtool -c" output afterwards
#   - every HWRM_RING_CMPL_RING_CFG_AGGINT_PARAMS (0x53) request sent to firmware
#     (all fields from ring_id to enables, per ring)
#   - the number of ETHTOOL_MSG_COALESCE_NTF notifications ("ethtool --monitor")
# usage: IF=eno1np0 ethtool_equiv.sh [U ...]
DIR=$(cd "$(dirname "$0")" && pwd)
IF=${IF:-eno1np0}
T=/sys/kernel/tracing
RUN="$DIR/netdev_coal_run.sh"
OUT=${OUT:-$DIR/equiv_out}
CASES=${*:-"50 0 200 400 1000 2000 4000 1"}

base_u=$(ethtool -c $IF | awk '/^rx-usecs:/{print $2}')
mkdir -p $OUT

cleanup() {
	[ -n "$MON" ] && kill $MON 2>/dev/null
	echo 0 > $T/events/kprobes/hwrmfull/enable 2>/dev/null
	echo "-:hwrmfull" >> $T/kprobe_events 2>/dev/null
	echo > $T/trace; echo 1 > $T/tracing_on
	ethtool -C $IF rx-usecs $base_u
	rm -f /sys/fs/bpf/netdev_coal
	echo "restored $IF to rx-usecs $base_u"
}
trap cleanup EXIT

echo 0 > $T/tracing_on; echo > $T/trace; echo nop > $T/current_tracer
echo 'p:hwrmfull bnxt_en:hwrm_req_send type=+0(%si):x16 ring=+16(%si):u16 flags=+18(%si):x16 dma_aggr=+20(%si):u16 dma_aggr_int=+22(%si):u16 dma_tmr=+24(%si):u16 dma_tmr_int=+26(%si):u16 tmr_min=+28(%si):u16 tmr_max=+30(%si):u16 aggr_int=+32(%si):u16 enables=+34(%si):x16' >> $T/kprobe_events
echo 1 > $T/events/kprobes/hwrmfull/enable

stdbuf -oL ethtool --monitor > $OUT/monitor.log 2>&1 &
MON=$!
sleep 0.5

# capture <tag> <cmd...>: reset to baseline, run cmd, record ethtool -c, fw msgs, ntf count
capture() {
	local tag=$1; shift
	ethtool -C $IF rx-usecs $base_u 2>/dev/null
	sleep 0.3
	local n0=$(grep -c "^Coalesce parameters for $IF" $OUT/monitor.log)
	echo > $T/trace; echo 1 > $T/tracing_on
	"$@" > $OUT/$tag.rc 2>&1
	sleep ${WAIT:-0.5}
	echo 0 > $T/tracing_on
	local n1=$(grep -c "^Coalesce parameters for $IF" $OUT/monitor.log)
	ethtool -c $IF > $OUT/$tag.show
	grep -oE 'type=0x53 .*' $T/trace | sort > $OUT/$tag.fw
	echo "$((n1 - n0))" > $OUT/$tag.ntf
}

fail=0
for c in $CASES; do
	u=$c
	capture eth_${u} ethtool -C $IF rx-usecs $u
	capture req_${u} $RUN $IF request $u
	capture set_${u} $RUN $IF set $u
	for k in set req; do
		d=""
		cmp -s $OUT/eth_${u}.show $OUT/${k}_${u}.show || d+=" ethtool-c"
		cmp -s $OUT/eth_${u}.fw $OUT/${k}_${u}.fw || d+=" fw-msgs"
		cmp -s $OUT/eth_${u}.ntf $OUT/${k}_${u}.ntf || d+=" ntf"
		printf '%-9s %-7s ethtool:[%s rc=%s fw=%s ntf=%s]  %s:[%s fw=%s ntf=%s]  %s\n' \
			"$u" "$k" \
			"$(awk '/^rx-usecs:/{print $2}' $OUT/eth_${u}.show)" \
			"$(grep -q . $OUT/eth_${u}.rc && echo err || echo 0)" \
			"$(wc -l < $OUT/eth_${u}.fw)" "$(cat $OUT/eth_${u}.ntf)" \
			"$k" "$(tr -d '\n' < $OUT/${k}_${u}.rc)" \
			"$(wc -l < $OUT/${k}_${u}.fw)" "$(cat $OUT/${k}_${u}.ntf)" \
			"${d:-SAME}"
		[ -n "$d" ] && fail=1
	done
done

# A request must not be dropped as a duplicate after ethtool changed the NIC.
echo; echo "after ethtool: BPF set 70, ethtool -C rx-usecs $base_u, BPF request 70"
$RUN $IF set 70 >/dev/null
ethtool -C $IF rx-usecs $base_u
$RUN $IF request 70 >/dev/null; sleep 0.5
got=$(ethtool -c $IF | awk '/^rx-usecs:/{print $2}')
echo "  ethtool -c rx-usecs: $got (want 70)"
[ "$got" = 70 ] || fail=1

[ $fail = 0 ] && echo "PASS: kfuncs match ethtool -C" || echo "DIFF: see $OUT"
