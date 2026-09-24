# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Research Linux tree (6.17-rc1 base). BPF struct_ops governors for cpuidle
(`drivers/cpuidle/governors/ext.c`) and cpufreq (`drivers/cpufreq/cpufreq_ext.c`), plus BPF
kfuncs that let those governors control NIC RX interrupt (NAPI) coalescing. There are more
detailed guides in `drivers/net/ethernet/broadcom/CLAUDE.md` and
`drivers/net/ethernet/broadcom/bnxt/CLAUDE.md`.

## Project state (as of 2026-09-24)

**Goal:** let BPF (a userspace-driven sweep first, then the cpuidle/cpufreq ext governors)
change the RX coalescing timer (rx-usecs only) on eno1np0 exactly like
`ethtool -C eno1np0 rx-usecs U`. The experiments live in the MOext repo (see below).

**Branch:** `napi_kfuncs_from_dbgprint`. The first version of the kfuncs is committed
(`86ddc216141e add generic coalescing kfunc`). Committed on top of it (2026-09-24):
`8b4485753344 cpufreq_ext: fix NULL deref in ext_disable() after governor exit` and
`bpf_coalesce: rx-usecs only, track ethtool changes`, which contain:
- `net/ethtool/bpf_coalesce.c`: the kfuncs now take **rx-usecs only**
  (`request(ifindex, usecs)`, `set(ifindex, usecs)`, `get(ifindex, &usecs)`); rx-frames is
  passed through unchanged. `request` accepts rx-usecs 0 like ethtool. New
  `netdev_coal_sync()` is called from `ethnl_notify()` for `ETHTOOL_MSG_COALESCE_NTF`
  (`net/ethtool/netlink.{c,h}`), so after any `ethtool -C` (netlink or ioctl) the slot holds
  the driver's real rx-usecs. This fixed a bug where `request` silently dropped a value that
  BPF had applied before and ethtool had since changed, and made `get` track ethtool. The
  apply result is now recorded under rtnl.
- `drivers/cpufreq/cpufreq_ext.c`: `ext_disable()` checks `ext_global_tuner` for NULL, and
  `ext_gov_init()`/`ext_gov_exit()` hold `ext_mutex`. Before, unregistering the BPF governor
  and then switching `scaling_governor` away quickly oopsed in `ext_disable()` (the link is
  released from a workqueue, after `ext_gov_exit()` freed the tuner) with `ext_mutex` held,
  so every later registration hung in D state until reboot.
- Test programs and scripts updated for the usecs-only API (`progs/netdev_coal_syscall.c`,
  `cpufreq_ext_coal.c`, `cpuidle_ext_coal.c`, `netdev_coal/*`). New:
  `netdev_coal/netdev_coal_run.py` (runs the syscall prog with `BPF_PROG_TEST_RUN` via
  ctypes, because `bpftool prog run` gets `-EINVAL` for syscall programs) and
  `netdev_coal/ethtool_equiv.sh` (compares `ethtool -C` with `set`/`request`: full
  `ethtool -c`, every firmware 0x53 message, COALESCE_NTF count).
- `Documentation/bpf/netdev-coalesce-kfuncs.md` updated.
- `netdev_coal/MOEXT_PROMPT.md`: the prompt for the Claude session in mibna001's MOext.

**Booted and verified (build #3, 2026-09-24):** release `6.17.0-rc1-moext-extgovs-napi+`,
`uname -v` is `#3 SMP PREEMPT_DYNAMIC Thu Sep 24 18:53:23 UTC 2026`.
- `verify_netdev_coal.sh`: PASS (set/request/get, dedup, rx-usecs 0, tracking of `ethtool -C`,
  burst, `-ENODEV`, adaptive-rx → `-EBUSY`; rx-frames/CQE mode/link unchanged).
- `ethtool_equiv.sh`: PASS. `set` and `request` match `ethtool -C IF rx-usecs U` for
  0/1/50/200/400/1000/2000/4000 (full `ethtool -c`, all 24 firmware messages, 1 NTF), and a
  request after an ethtool change is applied.
- `cpufreq_coal_test.sh async` and `sync`: PASS (sync calls `set` from `get_next_freq` in the
  dbs worker).
- cpufreq_ext race: register → `rm` link → immediate `echo schedutil` ×3, then register again:
  no oops, no hang.

The previous build is backed up as
`/boot/{vmlinuz,initrd.img,System.map,config}-6.17.0-rc1-moext-extgovs-napi+.pre-usecs` and
`/lib/modules/6.17.0-rc1-moext-extgovs-napi+.pre-usecs` (GRUB has a `.pre-usecs` entry); they
can be deleted when the user is happy. GRUB is `saved`, hidden menu, timeout 0, saved entry
= the `-napi+` advanced entry. Fallback via IPMI/serial: 5.15.0-185-generic.

**Next steps:**
1. The live cpuidle test (`cpuidle_ext_coal.o`) has no script; it replaces the idle governor
   on all CPUs, so ask the user first.
2. The user moves on to the MOext session (`netdev_coal/MOEXT_PROMPT.md`). Before it, as cc:
   `sudo install -m755 tools/bpf/bpftool/bpftool /usr/local/sbin/bpftool` and
   `sudo apt install libbpf-dev`.

## MOext

The experiments are in the MOext repo. **Never modify `/home/cc/MOext`**; the user edits
MOext only as user `mibna001` (`/home/mibna001/MOext`, on `main`). Read it with `git show` /
`git archive`, and for mibna001's checkout use `git -c safe.directory=/home/mibna001/MOext`.
- `main/experiments/vswarm_hotelapp_napi_coalescing/`: the ethtool sweep
  (`napi_knobs.py`: `ethtool -C IF adaptive-rx off`, then `ethtool -C IF rx-usecs U`;
  `config.json` `coalesce_ms` 0..4, i.e. rx-usecs 0..4000).
- Branch `analysis/power-saving-potential` (`036a4dd`, not in main): the old tg3 TC kfunc
  version. It attached to eno1np0 (bnxt), so `bpf_tg3_set_rx_coalesce` returned `-EINVAL`
  on every packet and never changed anything. Don't build on it.

## Machine facts (moext-baremetal-instance)

- **eno1np0:** bnxt_en, BCM57414, the **only uplink** (DHCP 10.191.130.215, default route).
  Coalescing as found: rx-usecs 10, rx-frames 15, tx 28/30, adaptive-rx off, CQE mode on,
  stats-block-usecs 1000000. 24 combined rings, so one change sends 24 firmware messages.
  eno2np1 is the second port, down.
- **No tg3 NIC** (no eno3/eno4) on this machine; the tg3 module is not loaded.
- **CPUs:** 96, `intel_cpufreq` (`intel_pstate=passive`), per-CPU policies, default
  governor `schedutil`, cpuidle `menu`; `ext` is available for both.
- **Tools:** clang 14 installed from apt (2026-09-24). No libbpf-dev (`/usr/include/bpf`
  missing). bpftool v7.7.0 built at `tools/bpf/bpftool/bpftool`; mibna001 can't read
  `/home/cc`, so copy it (e.g. `/usr/local/sbin`) for the MOext session.

## Findings to keep

(tg3 findings are from an earlier machine that had BCM5720 ports.)

- **`ethtool -C` on tg3:** `tg3_set_coalesce → __tg3_set_coalesce → tg3_coal_rx_init →
  tg3_write_rx_coalesce_vector` writes MMIO to `HOSTCC_*_VEC1 + q*0x18` (0x3d80 usecs,
  0x3d88 frames). It never sleeps, and it rejects usecs 0 or above 1023 and frames above 255.
- **`ethtool -C` on bnxt:** `bnxt_set_coalesce → bnxt_hwrm_set_coal` sends
  `HWRM_RING_CMPL_RING_CFG_AGGINT_PARAMS` (0x53) once per ring (24 on eno1np0 here). Timer values
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
  pinned, or it unregisters when bpftool exits. `rm -r LINK_DIR` unregisters it, but
  asynchronously: wait until `bpftool struct_ops show name <map>` fails before changing
  `scaling_governor`.
- `bpftool prog run` can't run `SEC("syscall")` programs (the kernel rejects `repeat` and
  `ctx_out`). Use `netdev_coal/netdev_coal_run.py`.
- **BPF build:** `clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -mcpu=v3 -I
  tools/testing/selftests/bpf/include -I tools/include/uapi -I
  tools/bpf/bpftool/libbpf/include` (the libbpf headers generated by the bpftool build; the
  system has none). Don't add `-I tools/lib`: it lacks the generated `bpf_helper_defs.h`. Programs use
  `BPF_NO_KFUNC_PROTOTYPES` and `extern ... __ksym`, against the committed `vmlinux.h`.
- **Kernel build:** `make -j$(nproc)` takes a long time for a full build. Run it in the
  background.
- **Tracing recipes:**
  - `coal_trace.sh` (tg3): function_graph on `ethnl_set_coalesce`, plus
    `p:tg3wr tg3:tg3_write32 off=%si val=%dx`.
  - `bnxt_coal_trace.sh`: kprobe on `bnxt_en:hwrm_req_send`, decoding the request at `%si`.
    Byte offsets: `ring_id` +16, `int_lat_tmr_max` +30, `num_cmpl_aggr_int` +32.
