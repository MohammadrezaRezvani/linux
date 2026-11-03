# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Context

This is the Broadcom NetXtreme-C/E (`bnxt_en`) driver inside a research Linux tree
(6.17-rc1 base, `/home/cc/research/linux`). The local branches add BPF extensibility
to kernel policy: cpuidle/cpufreq `ext` governors, and BPF kfuncs that tune NIC
interrupt coalescing. RX coalescing on bnxt (e.g. eno1np0) is controlled through the
**driver-independent** kfuncs in `net/ethtool/bpf_coalesce.c`
(`Documentation/bpf/netdev-coalesce-kfuncs.md`). They call `bnxt_set_coalesce` through
`ethtool_ops` exactly as `ethtool -C` does, so bnxt itself has no kfunc code. Only add
bnxt-specific kfuncs for things `ethtool_ops` can't express (e.g. per-ring coalescing via
`bnxt_hwrm_set_ring_coal`). The tg3 driver-local kfuncs (commit `eca9332f3c41`,
`Documentation/bpf/tg3-coalesce-experiment.md`) are the older TC-only prototype.
All of this is under `#ifdef CONFIG_BPF_SYSCALL`.

## Build / load / test

Run from the kernel root (`.config` has `CONFIG_BNXT=m` with SRIOV, FLOWER_OFFLOAD, DCB and HWMON enabled):

```bash
cd /home/cc/research/linux
make -j$(nproc) M=drivers/net/ethernet/broadcom/bnxt modules   # builds bnxt_en.ko
sudo rmmod bnxt_re 2>/dev/null; sudo rmmod bnxt_en; sudo insmod drivers/net/ethernet/broadcom/bnxt/bnxt_en.ko
scripts/checkpatch.pl -f drivers/net/ethernet/broadcom/bnxt/bnxt.c   # style check
```

`bnxt_re` (RoCE, `drivers/infiniband/hw/bnxt_re`) binds through the auxiliary bus (`bnxt_ulp.c`), so unload it first.
Kfunc BTF needs `CONFIG_DEBUG_INFO_BTF_MODULES=y`. Once a BPF program references a module's kfuncs, `rmmod` fails until that program is detached.

BPF test programs go in `tools/testing/selftests/bpf/progs/`. They are not wired into `run_tests.sh`.
Build and attach them by hand, as the tg3 doc does:
```bash
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -mcpu=v3 \
  -I tools/testing/selftests/bpf/include -I tools/include/uapi \
  -c tools/testing/selftests/bpf/progs/<prog>.c -o <prog>.o
sudo tc qdisc add dev $IF clsact
sudo tc filter add dev $IF egress bpf da obj <prog>.o sec tc
ethtool -c $IF      # driver cache (bp->rx_coal / tx_coal)
```
Test programs use `#define BPF_NO_KFUNC_PROTOTYPES`, declare kfuncs `extern ... __ksym`, and use `tools/testing/selftests/bpf/include/vmlinux.h`.
That header is committed. Regenerate it after kernel changes with `bpftool btf dump file vmlinux format c`.

## Architecture

The module is `bnxt_en.ko`. The file list is in `Makefile`; `bnxt_tc.o`, `bnxt_debugfs.o` and `bnxt_hwmon.o` are built only when their config options are set.
- `bnxt.c` (~17k lines) holds the core driver: PCI probe, `bnxt_netdev_ops`, ring alloc/free, NAPI poll, IRQs, open/close, the `bnxt_sp_task` slow-path worker, `module_init(bnxt_init)`.
- `bnxt.h` holds all core structs (`struct bnxt`, `bnxt_napi`, `bnxt_cp_ring_info`, `bnxt_coal`, ...).
- `bnxt_hwrm.c/.h` implement HWRM, the request/response channel to NIC firmware. Most hardware configuration is a firmware message, not an MMIO write.
  Message structs and constants live in `include/linux/bnxt/hsi.h`.
- `bnxt_ethtool.c` holds ethtool ops. `bnxt_dim.c` holds the net_dim worker. The remaining files cover SR-IOV, devlink, TC flower, PTP, XDP, VF reps, DCB, coredump and the ULP/RoCE bridge.

### HWRM pattern
```c
rc = hwrm_req_init(bp, req, HWRM_XXX);   // allocates DMA request (GFP_KERNEL)
/* fill req fields (little-endian: cpu_to_le16/32) */
hwrm_req_hold(bp, req);                  // optional: keep req/resp across multiple sends
rc = hwrm_req_send(bp, req);             // takes bp->hwrm_cmd_lock mutex, sleeps polling firmware
hwrm_req_drop(bp, req);                  // only if held
```
**HWRM calls sleep.** They cannot run in atomic context: TC/XDP BPF programs, NAPI poll, or IRQ handlers.
Code in atomic context stores the new values and defers the firmware call to process context.
Existing ways to do that:
- Queue a work item. `bnxt_dim_work` runs `bnxt_hwrm_set_ring_coal` from a workqueue.
- Set an `sp_event` bit and call `bnxt_queue_sp_work()`. `bnxt_sp_task` then handles it, e.g. `BNXT_RING_COAL_NOW_SP_EVENT`.

### Interrupt coalescing (the main area of research changes)
- Global config: `bp->rx_coal` and `bp->tx_coal` (`struct bnxt_coal`). Values are in usecs and buffers; `coal_bufs` is in units of `bufs_per_record`.
  Firmware limits are in `bp->coal_cap`, read by `bnxt_hwrm_coal_params_qcaps()`.
  `bnxt_hwrm_set_coal_params()` converts to firmware timer units and clamps to those limits.
- `bnxt_hwrm_set_coal(bp)` programs every completion ring from the global config. It runs on `bnxt_open`/reset and from `bnxt_set_coalesce` (ethtool -C).
  Anything else that changes coalescing gets overwritten by those paths.
- Per-ring: `bnapi->cp_ring.rx_ring_coal` plus `bnxt_hwrm_set_ring_coal(bp, bnapi)`. This is the path adaptive (DIM) mode uses.
  DIM samples in NAPI poll (`net_dim()` when `BNXT_FLAG_DIM` is set) and programs hardware from `bnxt_dim_work`.
- Ring topology: `bp->bnapi[0..cp_nr_rings-1]`. A bnapi may have an rx_ring, one or more tx_rings, or both.
  `bnxt_cp_ring_for_rx/tx()` map a ring to its firmware completion ring id.
  P5+ chips (`BNXT_FLAG_CHIP_P5_PLUS`) put an NQ above the per-ring CQs, which `__bnxt_hwrm_set_coal_nq` configures.

### Locking and state
- The netdev sets `dev->request_ops_lock = true`, so ndo and ethtool ops run under the netdev instance lock (`netdev_lock`, `netdev_ops_assert_locked`).
- `test_bit(BNXT_STATE_OPEN, &bp->state)` means rings exist in firmware. Check it before sending ring-level HWRM.
  Firmware reset, PCI error recovery and `in_reset` states can remove rings while code runs.
