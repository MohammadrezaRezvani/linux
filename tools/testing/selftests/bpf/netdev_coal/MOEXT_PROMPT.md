Add a BPF-kfunc version of main/experiments/vswarm_hotelapp_napi_coalescing that changes the
RX coalescing timer exactly the way the existing `ethtool -C` runner does, using the kernel
kfunc bpf_netdev_set_rx_coalesce instead of `ethtool -C IF rx-usecs U`. Work on a new branch
from main: experiment/netdev-bpf-coalescing. Do not modify napi_knobs.py or
run_vswarm_napi_coalescing.py. Do not use the tg3 kfuncs or the TC design from
origin/analysis/power-saving-potential (036a4dd). You may read that commit for structure only.
It never worked on this server: it attached to eno1np0 (bnxt), so the tg3 kfunc returned
-EINVAL on every packet.

Kernel facts (running kernel 6.17.0-rc1-moext-extgovs-napi+, verified against ethtool -C:
identical `ethtool -c` output, identical firmware messages to all 24 rings, one COALESCE_NTF):
- int bpf_netdev_set_rx_coalesce(u32 ifindex, u32 rx_usecs)       KF_SLEEPABLE
- int bpf_netdev_get_rx_coalesce(u32 ifindex, u32 *rx_usecs)
- int bpf_netdev_request_rx_coalesce(u32 ifindex, u32 rx_usecs)   (queued, for governors)
  They are registered for SYSCALL, STRUCT_OPS, TRACING and SCHED_CLS programs. set() runs the
  same path as `ethtool -C IF rx-usecs U`: only rx-usecs changes (rx-frames, tx, CQE mode are
  untouched), rx-usecs 0 is allowed, and if the value is already set, nothing is sent to the
  NIC. It returns 0, -EBUSY if adaptive-rx is on, -ENODEV, -EOPNOTSUPP, or the driver's error.
- Server NIC: eno1np0, bnxt_en BCM57414, 24 combined rings, adaptive-rx off.

Build:
1. main/bpf/napi_coalescing/netdev_rx_coal/netdev_coal.bpf.c: one SEC("syscall") program. Its
   ctx is struct { __u32 ifindex, rx_usecs, mode; __s32 ret; __u32 got_usecs; } (20 bytes,
   little endian). mode 0 calls set() and mode 2 calls get(). Declare the kfuncs as
   `extern int ... __ksym;`. Only bpf/bpf_helpers.h is needed (no vmlinux.h). Build with
   clang -O2 -g -target bpf -mcpu=v3, plus a Makefile and a target in main/Makefile.
2. napi_knobs_netdev_bpf.py, with the same API as napi_knobs.py (snapshot / apply_profile /
   restore_from_snapshot, same CommandResult logging):
   - Load and pin once: bpftool prog load netdev_coal.bpf.o /sys/fs/bpf/moext_netdev_coal
   - Run it with the bpf() syscall from Python ctypes, NOT `bpftool prog run` (the kernel
     rejects that with EINVAL for syscall programs because bpftool sets repeat=1 and ctx_out).
     Use syscall 321: BPF_OBJ_GET (cmd 7, attr struct.pack("<QIIi", path_ptr, 0, 0, 0)) returns
     the prog fd. Then BPF_PROG_TEST_RUN (cmd 10, 80-byte attr: prog_fd u32 at offset 0,
     ctx_size_in u32 at 40, ctx_in pointer u64 at 48, everything else 0). The program writes
     ret/got_usecs back into the ctx_in buffer. Create that buffer with an exact length:
     ctypes.create_string_buffer(data, len(data)).
   - Same order as the ethtool runner: run `ethtool -C IF adaptive-rx off` once in setup (this
     stays ethtool; the kfunc refuses with -EBUSY while adaptive-rx is on). Then for each cell
     call set(ifindex, rx_usecs). Do not clamp rx_usecs 0 to 1; the kfunc accepts 0 like
     ethtool does.
   - ret != 0 means the apply failed. Record ret, and read `ethtool -c` afterwards; its rx-usecs
     must equal the requested value. get() also reports the device's current rx-usecs (it
     follows ethtool changes too) and can be logged as a second check.
   - Restore from the snapshot with ethtool in `finally`, as napi_knobs.py does.
   - Preflight (exit rc=1 before touching the NIC): object file exists; /proc/kallsyms has
     bpf_netdev_set_rx_coalesce; bpftool is a real binary (not the Ubuntu /usr/sbin wrapper);
     set() with the current rx-usecs returns 0.
3. run_vswarm_napi_coalescing_netdev_bpf.py: copy of the ethtool runner using the new backend,
   run IDs prefixed with bpfset_, log prefix [napi-coal-bpf], output in the same format so
   the plotting and consolidation scripts work unchanged. Record the method
   ("bpf_netdev_set_rx_coalesce") in applied_profile.json and summary.json.
4. A/B validation, documented in the README: run one cell (e.g. rps 300, coalesce 0.2 ms) with
   both runners. NIC IRQ rate and `ethtool -c` must match within noise before running the full
   config.json sweep.

Do not: call a kfunc per packet from TC; use bpf_netdev_request_rx_coalesce for the sweep (it
is asynchronous and meant for governors); load cpufreq/cpuidle ext governors in this experiment.
Later governor work: after unregistering a cpufreq ext struct_ops, wait until
`bpftool struct_ops show name <map>` fails before changing scaling_governor.

Prerequisites on this server: clang 14 is installed; libbpf headers are missing
(sudo apt install libbpf-dev); use bpftool v7.7 at /usr/local/sbin/bpftool (copied from the
kernel tree, since /usr/sbin/bpftool is the Ubuntu wrapper and doesn't work on this kernel).
