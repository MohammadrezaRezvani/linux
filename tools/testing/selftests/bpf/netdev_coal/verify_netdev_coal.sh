#!/bin/bash
# Post-boot verification of net/ethtool/bpf_coalesce.c. Run as root.
# Restores eno1np0 rx-usecs/rx-frames and adaptive-rx on exit.
DIR=$(cd "$(dirname "$0")" && pwd)
export BPFTOOL=${BPFTOOL:-/home/cc/research/linux/tools/bpf/bpftool/bpftool}
IF=${IF:-eno1np0}
TG3=${TG3:-eno3}
T=/sys/kernel/tracing
RUN="$DIR/netdev_coal_run.sh"

orig_u=$(ethtool -c $IF | awk '/^rx-usecs:/{print $2}')
orig_f=$(ethtool -c $IF | awk '/^rx-frames:/{print $2}')
orig_a=$(ethtool -c $IF | awk '/^Adaptive RX:/{print $3}')

cleanup() {
	echo 0 > $T/tracing_on; echo nop > $T/current_tracer; echo > $T/set_graph_function
	for e in hwrmsend workfn applyfn; do
		echo 0 > $T/events/kprobes/$e/enable 2>/dev/null
		echo "-:$e" >> $T/kprobe_events 2>/dev/null
	done
	echo > $T/trace; echo 1 > $T/tracing_on
	ethtool -C $IF adaptive-rx ${orig_a:-off} 2>/dev/null
	ethtool -C $IF rx-usecs $orig_u rx-frames $orig_f
	rm -f /sys/fs/bpf/netdev_coal
	echo "restored $IF to $(ethtool -c $IF | awk '/^rx-usecs:/{u=$2}/^rx-frames:/{f=$2}END{print u"/"f}') adaptive-rx=${orig_a:-off}"
}
trap cleanup EXIT

hdr() { echo; echo "=== $*"; }
cache() { ethtool -c $1 | awk '/^rx-usecs:/{u=$2}/^rx-frames:/{f=$2}/^CQE mode RX:/{c=$4}END{print u"/"f" cqe_rx="c}'; }
probes() {
	echo 0 > $T/tracing_on; echo > $T/trace; echo nop > $T/current_tracer
	echo 'p:hwrmsend bnxt_en:hwrm_req_send type=+0(%si):x16 ring=+16(%si):u16 tmr_max=+30(%si):u16 aggr_int=+32(%si):u16' >> $T/kprobe_events
	echo 'p:workfn netdev_coal_workfn' >> $T/kprobe_events
	echo 'p:applyfn netdev_apply_rx_coalesce usecs=%si:u32 frames=%dx:u32' >> $T/kprobe_events
	for e in hwrmsend workfn applyfn; do echo 1 > $T/events/kprobes/$e/enable; done
}
summ() {
	local n=$(grep -c 'hwrmsend:.*type=0x53' $T/trace)
	local p=$(grep -oE 'type=0x53 ring=[0-9]+ tmr_max=[0-9]+ aggr_int=[0-9]+' $T/trace | sed -E 's/ring=[0-9]+ //' | sort | uniq -c | tr '\n' ';')
	local w=$(grep -c 'workfn:' $T/trace)
	local a=$(grep -oE 'applyfn:.*' $T/trace | sed 's/.*usecs=/usecs=/' | tr '\n' ';')
	local irqoff=$(grep 'hwrmsend:' $T/trace | awk '{print $3}' | grep -c '^d' || true)
	echo "  fw 0x53 sends=$n [$p] worker_runs=$w apply=[$a] sends_with_irqs_off=$irqoff"
}
go() { echo > $T/trace; echo 1 > $T/tracing_on; "$@"; sleep ${WAIT:-0.3}; echo 0 > $T/tracing_on; }

hdr "2. kfuncs in vmlinux BTF"
$BPFTOOL btf dump file /sys/kernel/btf/vmlinux | grep -oE "FUNC 'bpf_netdev_[a-z_]+'" | sort -u
[ -x "$RUN" ] || { echo "missing $RUN"; exit 1; }
probes

hdr "3. sleepable set 50/10 on $IF (link must stay up, CQE mode unchanged)"
before_cqe=$(cache $IF)
go $RUN $IF set 50 10
echo "  before: $before_cqe  after: $(cache $IF)  link: $(cat /sys/class/net/$IF/operstate)"
summ

hdr "4a. request 100/20 (worker applies it)"
go $RUN $IF request 100 20
echo "  get: $($RUN $IF get)   ethtool: $(cache $IF)"
summ

hdr "4b. duplicate request 100/20 x50 (worker must not run)"
go bash -c "for i in \$(seq 50); do $RUN $IF request 100 20 >/dev/null; done"
summ

hdr "4c. burst 8/1 then 120/25 (last value wins)"
WAIT=0.5 go bash -c "$RUN $IF request 8 1; $RUN $IF request 120 25"
echo "  get: $($RUN $IF get)   ethtool: $(cache $IF)"
summ

hdr "5. driver independence: set 50/10 on $TG3 (tg3)"
if [ -e /sys/class/net/$TG3 ]; then
	tu=$(ethtool -c $TG3 | awk '/^rx-usecs:/{print $2}'); tf=$(ethtool -c $TG3 | awk '/^rx-frames:/{print $2}')
	echo "  $($RUN $TG3 set 50 10)  ethtool: $(cache $TG3)"
	ethtool -d $TG3 raw on > $DIR/tg3regs.bin 2>/dev/null && \
		echo "  HW 0x3d80/0x3d88 = $(od -An -tu4 -j $((0x3d80)) -N4 $DIR/tg3regs.bin | tr -d ' ')/$(od -An -tu4 -j $((0x3d88)) -N4 $DIR/tg3regs.bin | tr -d ' ')"
	ethtool -C $TG3 rx-usecs $tu rx-frames $tf
fi

hdr "6. error cases"
echo "  bogus ifindex:  $(bash -c 'IDX=99999; python3 -c "import struct,sys; sys.stdout.buffer.write(struct.pack(\"<IIIIiII\",99999,50,10,0,0,0,0))" > '$DIR'/ctx_in.bin; $BPFTOOL prog run pinned /sys/fs/bpf/netdev_coal ctx_in '$DIR'/ctx_in.bin ctx_out '$DIR'/ctx_out.bin >/dev/null; python3 -c "import struct; print(struct.unpack(\"<IIIIiII\",open(\"'$DIR'/ctx_out.bin\",\"rb\").read()[:28])[4])"') (expect -19 ENODEV)"
ethtool -C $IF adaptive-rx on
echo "  adaptive-rx on: set -> $($RUN $IF set 60 12) (expect -16 EBUSY)"
ethtool -C $IF adaptive-rx ${orig_a:-off}
