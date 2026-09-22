# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Research Linux tree (6.17-rc1 base). BPF struct_ops governors for cpuidle
(`drivers/cpuidle/governors/ext.c`) and cpufreq (`drivers/cpufreq/cpufreq_ext.c`), plus BPF
kfuncs that let those governors control NIC RX interrupt (NAPI) coalescing. There are more
detailed guides in `drivers/net/ethernet/broadcom/CLAUDE.md` and
`drivers/net/ethernet/broadcom/bnxt/CLAUDE.md`.

## Project state (as of 2026-09-22)

**Goal:** let the cpuidle/cpufreq ext governors change RX coalescing on eno1np0 (bnxt) from BPF.

**Branch:** `napi_kfuncs_from_dbgprint`. **Nothing from this work is committed yet.**
- Modified: `drivers/cpuidle/governors/ext.c`, `net/ethtool/Makefile`,
  `tools/testing/selftests/bpf/progs/cpuidle_gov_ext.c`.
- New: `net/ethtool/bpf_coalesce.c`, `Documentation/bpf/netdev-coalesce-kfuncs.md`,
  `tools/testing/selftests/bpf/progs/{netdev_coal_syscall,cpuidle_ext_coal,cpufreq_ext_coal}.c`,
  `tools/testing/selftests/bpf/netdev_coal/` (test scripts), and the CLAUDE.md files.

**Implemented** (design and API: `Documentation/bpf/netdev-coalesce-kfuncs.md`):
- `net/ethtool/bpf_coalesce.c` (built in) adds driver-independent kfuncs. They call
  `ethtool_ops->set_coalesce` the same way `ethtool -C` does, under rtnl, `netdev_lock_ops`
  and `ethnl_ops_begin`. They read the current settings first and change only rx-usecs and
  rx-frames.
  - `bpf_netdev_request_rx_coalesce(ifindex, usecs, frames)`: never sleeps, safe with IRQs
    off. Stores the request in an 8-slot table (raw spinlock), drops duplicates, and a worker
    on `system_unbound_wq` applies it.
  - `bpf_netdev_set_rx_coalesce(...)`: `KF_SLEEPABLE`, applies synchronously.
  - `bpf_netdev_get_rx_coalesce(ifindex, &u, &f)`: returns the last values applied through BPF.
  - Registered for STRUCT_OPS, SYSCALL, TRACING and SCHED_CLS. `request` and `get` return
    `-EBUSY` in NMI context.
- `cpuidle ext.c`, `ops_check_member`: rejects sleepable programs for `select`, `reflect` and
  `set_stop_tick`, which run in the idle loop with IRQs off. `cpuidle_gov_ext.c` was switched
  to `struct_ops/` sections for those three callbacks to match.
- The tg3 driver-local TC kfuncs from commit `eca9332f3c41` are unchanged; they are an older
  prototype.
- No bnxt changes.

**Built:** new code passes `checkpatch.pl` with 0 warnings and has no W=1 warnings. vmlinux BTF
has the 3 `bpf_netdev_*` kfuncs. `CONFIG_LOCALVERSION` was changed to
`-v6.17rc1-cstate-extgovs-napi`, so the release is `6.17.0-rc1-v6.17rc1-cstate-extgovs-napi+`.

**Installed but not yet booted at the time of writing:** `make modules_install
INSTALL_MOD_STRIP=1 && make install`. GRUB uses `GRUB_DEFAULT=saved` with a hidden menu and
timeout 0, and the saved entry is the top-level "Ubuntu" entry, which now points to `-napi+`.
So a plain reboot boots the new kernel, with no automatic fallback. The previous kernel is
`6.17.0-rc1-v6.17rc1-cstate-extgovs+` (in GRUB under Advanced options). The user has IPMI and
serial console (ttyS0) access. **Check `uname -r` first**: if it ends in `-napi+`, the new
kernel is running.

**Next steps (after booting `-napi+`):**
1. `tools/testing/selftests/bpf/netdev_coal/build.sh` builds the BPF objects.
2. `sudo tools/testing/selftests/bpf/netdev_coal/verify_netdev_coal.sh`: kfuncs present;
   synchronous set 50/10 on eno1np0 (expect 16 firmware `0x53` sends with `tmr_max=625` and
   `aggr_int=20`, link up, CQE mode unchanged); request, duplicate and burst behaviour; tg3
   eno3; error cases (`-ENODEV`, adaptive-rx → `-EBUSY`). It restores the original settings.
3. `sudo .../cpufreq_coal_test.sh async`, then `sync`. The script switches only cpu0's
   policy to `ext`, steps mode 0 → 1 → 0, checks frequency and coalescing, and prints
   PASS/FAIL. The kprobes on static functions (`netdev_apply_rx_coalesce`,
   `netdev_coal_workfn`) may fail if the compiler inlined them; check
   `/sys/kernel/tracing/available_filter_functions`.
4. There is no script yet for the live cpuidle test (`cpuidle_ext_coal.o`). Registering it
   replaces the idle governor on all CPUs, so ask the user before running it.
5. Then commit, when the user asks.

## Machine facts

- **eno1np0:** bnxt_en, BCM57412 (pre-P5), the **only uplink** (DHCP 10.52.2.129 and the
  default route). Original coalescing: rx 500/15, tx 28/30, adaptive-rx off, CQE mode on,
  stats-block-usecs 1000000. 16 combined rings.
- **eno3 / eno4:** tg3, BCM5720, **no carrier** (no cable), so no traffic is possible. They
  have 4 RX queues with RSS. eno3's original setting: rx 20/5.
- **CPUs:** 64, `intel_cpufreq` (`intel_pstate=passive`), per-CPU policies, default
  governor `schedutil`; `ext` is available.
- **Kernel config:** `CONFIG_CPU_FREQ_GOV_EXT=y`, `CONFIG_CPU_IDLE_GOV_EXT=y`,
  `CONFIG_DEBUG_INFO_BTF(_MODULES)=y`. Lockdep and `DEBUG_ATOMIC_SLEEP` are off.

## Findings to keep

- **`ethtool -C` on tg3:** `tg3_set_coalesce → __tg3_set_coalesce → tg3_coal_rx_init →
  tg3_write_rx_coalesce_vector` writes MMIO to `HOSTCC_*_VEC1 + q*0x18` (0x3d80 usecs,
  0x3d88 frames). It never sleeps, and it rejects usecs 0 or above 1023 and frames above 255.
- **`ethtool -C` on bnxt:** `bnxt_set_coalesce → bnxt_hwrm_set_coal` sends
  `HWRM_RING_CMPL_RING_CFG_AGGINT_PARAMS` (0x53) once per ring (16 on eno1np0). Timer values
  are in 80 ns units (50 µs → 625). Frames are multiplied by `bufs_per_record` (2). It sleeps
  (`GFP_KERNEL`, `hwrm_cmd_lock` mutex, `usleep_range`; about 2 ms total), so it can't be
  called from TC or idle context. Writing the BAR0 mailbox directly is unsafe because there's
  a single mailbox slot. On bnxt, changing `stats-block-usecs` closes and reopens the NIC.
  bnxt accepts 0 usecs (clamped to 1).
- **Traffic doesn't change the tg3 path.** No branch depends on link or traffic, since 5720 is
  `5705_PLUS`. The bnxt path was verified with traffic on eno1np0.
- **Contexts:** cpuidle `select`, `reflect` and `set_stop_tick` run with IRQs off.
  `enable`/`disable` run under `cpuidle_pause_and_lock` (process context). cpufreq
  `get_next_freq` runs in the dbs workqueue holding `policy_dbs->update_mutex`.
  `cpufreq_ext`'s `ext_reg` needs `scaling_governor=ext` on some policy first.

## Tooling gotchas

- `/usr/sbin/bpftool` is the Ubuntu wrapper and fails on this kernel. Use
  `tools/bpf/bpftool/bpftool` (v7.7.0).
- `bpftool struct_ops register OBJ LINK_DIR`: a `.struct_ops.link` registration must be
  pinned, or it unregisters when bpftool exits. `rm -r LINK_DIR` unregisters it.
- **BPF build:** `clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -mcpu=v3 -I
  tools/testing/selftests/bpf/include -I tools/include/uapi`. Don't add `-I tools/lib`: the
  in-tree libbpf lacks the generated `bpf_helper_defs.h`. Programs use
  `BPF_NO_KFUNC_PROTOTYPES` and `extern ... __ksym`, against the committed `vmlinux.h`.
- **Kernel build:** `make -j$(nproc)` takes a long time for a full build. Run it in the
  background.
- **Tracing recipes:**
  - `coal_trace.sh` (tg3): function_graph on `ethnl_set_coalesce`, plus
    `p:tg3wr tg3:tg3_write32 off=%si val=%dx`.
  - `bnxt_coal_trace.sh`: kprobe on `bnxt_en:hwrm_req_send`, decoding the request at `%si`.
    Byte offsets: `ring_id` +16, `int_lat_tmr_max` +30, `num_cmpl_aggr_int` +32.
