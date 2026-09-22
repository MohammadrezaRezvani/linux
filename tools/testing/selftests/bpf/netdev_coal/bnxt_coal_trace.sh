#!/bin/bash
# Trace ethtool -C on a bnxt_en port under load; decode HWRM_RING_CMPL_RING_CFG_AGGINT_PARAMS
# (0x53) requests sent to firmware. Offsets from include/linux/bnxt/hsi.h.
IF=${IF:-eno1np0}
T=/sys/kernel/tracing
OUT=$(dirname "$0")/bnxt_runs
mkdir -p "$OUT"
GW=$(ip route show default dev $IF | awk '{print $3; exit}')

setup() {
	echo 0 > $T/tracing_on; echo > $T/trace; echo nop > $T/current_tracer
	echo 'p:hwrmsend bnxt_en:hwrm_req_send type=+0(%si):x16 ring=+16(%si):u16 flags=+18(%si):x16 dma_aggr=+20(%si):u16 dma_aggr_int=+22(%si):u16 dma_tmr=+24(%si):u16 dma_tmr_int=+26(%si):u16 tmr_min=+28(%si):u16 tmr_max=+30(%si):u16 aggr_int=+32(%si):u16 en=+34(%si):x16' >> $T/kprobe_events
	echo 'r:hwrmret bnxt_en:hwrm_req_send rc=$retval:s32' >> $T/kprobe_events
	echo 1 > $T/events/kprobes/hwrmsend/enable
	echo 1 > $T/events/kprobes/hwrmret/enable
	echo ethnl_set_coalesce > $T/set_graph_function
	echo 7 > $T/max_graph_depth
	echo function_graph > $T/current_tracer
}
cleanup() {
	kill $PING 2>/dev/null
	echo 0 > $T/tracing_on; echo nop > $T/current_tracer; echo > $T/set_graph_function; echo 0 > $T/max_graph_depth
	echo 0 > $T/events/kprobes/hwrmsend/enable 2>/dev/null; echo 0 > $T/events/kprobes/hwrmret/enable 2>/dev/null
	echo '-:hwrmsend' >> $T/kprobe_events; echo '-:hwrmret' >> $T/kprobe_events
	echo > $T/trace; echo 1 > $T/tracing_on
}
trap cleanup EXIT
irqs() { grep "$IF" /proc/interrupts | awk '{for(i=2;i<=NF;i++) if ($i ~ /^[0-9]+$/) s+=$i} END{print s}'; }
rxp() { awk -v i="$IF:" '$1==i{print $3}' /proc/net/dev; }

ping -q -i 0.005 $GW > /dev/null 2>&1 & PING=$!
sleep 1
setup

for pair in "$@"; do
	u=${pair%/*}; f=${pair#*/}
	i0=$(irqs); p0=$(rxp)
	echo > $T/trace; echo 1 > $T/tracing_on
	ethtool -C $IF rx-usecs $u rx-frames $f 2>&1; rc=$?
	echo 0 > $T/tracing_on
	sleep 1
	i1=$(irqs); p1=$(rxp)
	tr=$OUT/trace_${u}_${f}.txt; cat $T/trace > $tr
	path=$(grep -oE '\b(ethnl_set_coalesce|__ethnl_set_coalesce[.a-z0-9]*|bnxt_[a-z_0-9]+|__bnxt_[a-z_0-9]+|hwrm_req_[a-z]+|__hwrm_[a-z_]+)\(' $tr | tr -d '(' | awk '!s[$0]++' | tr '\n' ' ')
	nsend=$(grep -c 'hwrmsend:' $tr)
	nfail=$(grep 'hwrmret:' $tr | grep -vc 'rc=0')
	cache=$(ethtool -c $IF | awk '/^rx-usecs:/{u=$2}/^rx-frames:/{f=$2}END{print u"/"f}')
	echo "=== set $u/$f  ethtool_rc=$rc  cache=$cache  load: +$((p1-p0)) rx pkts, +$((i1-i0)) irqs (~1s)"
	echo "  path: $path"
	echo "  HWRM 0x53 sends: $nsend  non-zero fw rc: $nfail"
	# distinct request payloads (ring id stripped) with counts
	grep -oE 'type=0x53 ring=[0-9]+ .*' $tr | sed -E 's/ring=[0-9]+ //' | sort | uniq -c | sed 's/^/   /'
	echo "  rings: $(grep -oE 'type=0x53 ring=[0-9]+' $tr | awk -F= '{print $3}' | tr '\n' ' ')"
done
