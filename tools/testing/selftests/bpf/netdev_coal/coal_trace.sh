#!/bin/bash
# For each (usecs frames) pair: trace ethtool -C path + tg3_write32 register writes,
# then read back HostCC RX registers from hardware via ethtool -d raw.
IF=${IF:-eno3}
T=/sys/kernel/tracing
OUT=$(dirname "$0")/runs
mkdir -p "$OUT"

setup() {
	echo 0 > $T/tracing_on; echo > $T/trace; echo nop > $T/current_tracer
	echo 'p:tg3wr tg3:tg3_write32 off=%si:x32 val=%dx:x32' >> $T/kprobe_events
	echo 1 > $T/events/kprobes/tg3wr/enable
	echo ethnl_set_coalesce > $T/set_graph_function
	echo function_graph > $T/current_tracer
}
cleanup() {
	echo 0 > $T/tracing_on; echo nop > $T/current_tracer; echo > $T/set_graph_function
	echo 0 > $T/events/kprobes/tg3wr/enable 2>/dev/null; echo '-:tg3wr' >> $T/kprobe_events
	echo > $T/trace; echo 1 > $T/tracing_on
}
trap cleanup EXIT
setup

rd() { od -An -tu4 -j $(($2)) -N4 "$1" | tr -d ' '; }

for pair in "$@"; do
	u=${pair%/*}; f=${pair#*/}
	echo > $T/trace; echo 1 > $T/tracing_on
	ethtool -C $IF rx-usecs $u rx-frames $f 2>&1; rc=$?
	echo 0 > $T/tracing_on
	tr=$OUT/trace_${u}_${f}.txt; cat $T/trace > $tr
	# call path: function names only, in order, deduped
	path=$(grep -oE '(ethnl_set_coalesce|__ethnl_set_coalesce[.a-z0-9]*|tg3_[a-z_0-9]+)' $tr | grep -v tg3_write32 | uniq | tr '\n' ' ')
	# RX-vector register writes (0x3d80 + q*0x18 ticks, +8 frames)
	w=$(grep -oE 'off=0x3d(80|88|98|a0|b0|b8|c8|d0) val=0x[0-9a-f]+' $tr | tr '\n' ' ')
	nw=$(grep -c 'tg3wr:' $tr)
	ethtool -d $IF raw on > $OUT/regs.bin
	hw=""; ok=1
	for q in 0 1 2 3; do
		t=$(rd $OUT/regs.bin $((0x3d80 + q*0x18))); fr=$(rd $OUT/regs.bin $((0x3d88 + q*0x18)))
		hw+="q$q=$t/$fr "
		[ "$t" = "$u" ] && [ "$fr" = "$f" ] || ok=0
	done
	cache=$(ethtool -c $IF | awk '/^rx-usecs:/{u=$2}/^rx-frames:/{f=$2}END{print u"/"f}')
	echo "=== set $u/$f  ethtool_rc=$rc  total_writes=$nw  cache=$cache"
	echo "  path:  $path"
	echo "  RX writes: $w"
	echo "  HW readback: $hw  match=$ok"
done
