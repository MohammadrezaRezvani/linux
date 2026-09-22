#!/bin/bash
# End-to-end test of cpufreq_ext_coal.o: the cpufreq ext governor changes
# CPU frequency and NIC RX coalescing together. Run as root on the new kernel.
#   cpufreq_coal_test.sh [async|sync]     (default async)
# async = request kfunc (worker applies it); sync = sleepable set kfunc
# called directly from get_next_freq.
# Only the decider CPU's policy is switched to "ext"; everything is restored
# on exit.
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
BPFTOOL=${BPFTOOL:-/home/cc/research/linux/tools/bpf/bpftool/bpftool}
VARIANT=${1:-async}
IF=${IF:-eno1np0}
CPU=${CPU:-0}
FAST="50 10"
SLOW="200 32"
INTERVAL_NS=100000000	# 100 ms
LINK=/sys/fs/bpf/cpufreq_coal
T=/sys/kernel/tracing
POL=/sys/devices/system/cpu/cpu$CPU/cpufreq
[ "$VARIANT" = sync ] && OBJ=$DIR/cpufreq_ext_coal_sync.o || OBJ=$DIR/cpufreq_ext_coal.o

orig_gov=$(cat $POL/scaling_governor)
orig_u=$(ethtool -c $IF | awk '/^rx-usecs:/{print $2}')
orig_f=$(ethtool -c $IF | awk '/^rx-frames:/{print $2}')
IDX=$(cat /sys/class/net/$IF/ifindex)
FAIL=0

cleanup() {
	echo 0 > $T/tracing_on
	for e in applyfn hwrmsend; do
		echo '!stacktrace:1' > $T/events/kprobes/$e/trigger 2>/dev/null
		echo 0 > $T/events/kprobes/$e/enable 2>/dev/null
		echo "-:$e" >> $T/kprobe_events 2>/dev/null
	done
	echo > $T/trace; echo 1 > $T/tracing_on
	rm -rf $LINK
	echo $orig_gov > $POL/scaling_governor
	ethtool -C $IF rx-usecs $orig_u rx-frames $orig_f
	echo "restored: cpu$CPU governor=$(cat $POL/scaling_governor) $IF rx=$(coal)"
	[ $FAIL = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL ($FAIL checks)"
}
trap cleanup EXIT

coal() { ethtool -c $IF | awk '/^rx-usecs:/{u=$2}/^rx-frames:/{f=$2}END{print u"/"f}'; }
check() { if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2 want $3"; FAIL=$((FAIL+1)); fi; }
# struct coal_cfg: 8 x u32 + u64
setcfg() {
	local hex
	hex=$(python3 -c "import struct; print(' '.join('%02x'%b for b in struct.pack('<IIIIIIIIQ',$IDX,$CPU,$1,${FAST% *},${FAST#* },${SLOW% *},${SLOW#* },0,$INTERVAL_NS)))")
	$BPFTOOL map update name coal_cfg key 0 0 0 0 value hex $hex
}
stats() { $BPFTOOL map dump name coal_stats -j | python3 -c "
import json,sys
v=json.load(sys.stdin)[0]['value']
print('  stats: calls=%d requests=%d last_rc=%d mode=%d usecs=%d frames=%d sync=%d' % (v['calls'],v['requests'],v['last_rc'],v['last_mode'],v['last_usecs'],v['last_frames'],v['sync']))"; }

echo "=== variant=$VARIANT obj=$(basename $OBJ) cpu$CPU $IF (ifindex $IDX)"
echo "  before: governor=$orig_gov freq=$(cat $POL/scaling_cur_freq) min=$(cat $POL/scaling_min_freq) max=$(cat $POL/scaling_max_freq) rx=$orig_u/$orig_f"

# probes: who calls netdev_apply_rx_coalesce, and firmware sends
echo 'p:applyfn netdev_apply_rx_coalesce usecs=%si:u32 frames=%dx:u32' >> $T/kprobe_events
echo 'p:hwrmsend bnxt_en:hwrm_req_send type=+0(%si):x16' >> $T/kprobe_events
echo 'stacktrace:1' > $T/events/kprobes/applyfn/trigger
echo 1 > $T/events/kprobes/applyfn/enable
echo 1 > $T/events/kprobes/hwrmsend/enable
echo > $T/trace

echo ext > $POL/scaling_governor || { echo "cannot select ext governor"; exit 1; }
$BPFTOOL struct_ops register $OBJ $LINK || { echo "register failed"; FAIL=1; exit 1; }
dmesg | grep -E "cpufreq_ext|ext_reg" | tail -1

echo "--- mode 0 (performance): expect max freq, rx $FAST"
setcfg 0; sleep 1.5
stats
check "cpu$CPU freq" "$(cat $POL/scaling_cur_freq)" "$(cat $POL/scaling_max_freq)"
check "$IF rx" "$(coal)" "${FAST// //}"

echo "--- mode 1 (powersave): expect min freq, rx $SLOW"
setcfg 1; sleep 1.5
stats
check "cpu$CPU freq" "$(cat $POL/scaling_cur_freq)" "$(cat $POL/scaling_min_freq)"
check "$IF rx" "$(coal)" "${SLOW// //}"

echo "--- mode 0 again"
setcfg 0; sleep 1.5
stats
check "$IF rx" "$(coal)" "${FAST// //}"

echo 0 > $T/tracing_on
echo "--- applies seen (should be 3, one per mode change, despite ~10 requests/s):"
grep -c 'applyfn:' $T/trace | sed 's/^/  /'
echo "  fw 0x53 sends: $(grep -c 'hwrmsend:.*type=0x53' $T/trace)"
echo "--- first caller of netdev_apply_rx_coalesce (async: netdev_coal_workfn; sync: dbs_work_handler/ext_gov_update):"
awk '/<stack trace>/{p=1;next} p&&/=>/{print "  "$0; if(++n>=12) exit} p&&!/=>/{if(n) exit}' $T/trace
