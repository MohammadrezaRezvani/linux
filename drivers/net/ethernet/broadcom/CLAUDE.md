# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Context

This directory holds the Broadcom Ethernet drivers inside a research Linux tree (6.17-rc1 base, kernel root `/home/cc/research/linux`).
The research adds BPF extensibility to kernel policy:
- cpuidle/cpufreq `ext` governors are struct_ops programs (`drivers/cpuidle/governors/ext.c`, `drivers/cpufreq/cpufreq_ext.c`).
- BPF kfuncs let those governors (and other BPF programs) tune NIC interrupt coalescing.

The main interface is the **driver-independent** kfuncs in `net/ethtool/bpf_coalesce.c` (`Documentation/bpf/netdev-coalesce-kfuncs.md`). They call `ethtool_ops->set_coalesce` for any NIC, so they need no code here. Use the non-sleeping `bpf_netdev_request_rx_coalesce` from cpuidle `select` (IRQs off), and the `KF_SLEEPABLE` `bpf_netdev_set_rx_coalesce` only from sleepable programs.

The older TC-only prototype is in **tg3** (commit `eca9332f3c41`, tested on BCM5720 `eno3`). Its design, register map and test log are in `Documentation/bpf/tg3-coalesce-experiment.md`. Read that doc before you change the kfuncs.
The bnxt port has its own guide, `bnxt/CLAUDE.md` (HWRM, coalescing paths, locking). Read that file for any work under `bnxt/`.

Most other drivers here (b44, bnx2, bnx2x, cnic, genet, bcmsysport, bgmac, asp2, bnge) are unmodified upstream code.

## Build / load

Run from the kernel root. In `.config`, `TIGON3`, `BNX2`, `BNX2X`, `CNIC`, `B44`, `BCMGENET`, `SYSTEMPORT` and `BNXT` are all `=m`, and `BNGE` is not set. `CONFIG_BPF_SYSCALL=y` and `CONFIG_DEBUG_INFO_BTF_MODULES=y` are set; kfunc BTF depends on both.

```bash
cd /home/cc/research/linux
make -j$(nproc) M=drivers/net/ethernet/broadcom modules          # all broadcom modules (tg3.ko etc.)
make -j$(nproc) M=drivers/net/ethernet/broadcom/bnxt modules     # bnxt only
sudo rmmod tg3; sudo insmod drivers/net/ethernet/broadcom/tg3.ko
scripts/checkpatch.pl -f drivers/net/ethernet/broadcom/tg3.c
```
`rmmod` fails while a loaded BPF program references the module's kfuncs. Detach the tc filter first, e.g. `tc qdisc del dev $IF clsact`.

## BPF kfunc tests

The test programs are in `tools/testing/selftests/bpf/progs/`: `tg3_coal_test.c` (global) and `tg3_coal_queue_test.c` (per queue). They are not part of `run_tests.sh`, so build and attach them by hand:

```bash
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -mcpu=v3 \
  -I tools/testing/selftests/bpf/include -I tools/include/uapi \
  -c tools/testing/selftests/bpf/progs/tg3_coal_test.c -o tg3_coal_test.o
sudo tc qdisc add dev $IF clsact
sudo tc filter add dev $IF egress bpf da obj tg3_coal_test.o sec tc   # section name is "tc", not the function name
ping -c 3 -I $IF 192.0.2.1                                           # triggers egress; works without carrier
ethtool -c $IF                                   # driver cache (tp->coal)
sudo ethtool -d $IF | rg '0x3d[89ab][0-9a-f]'    # per-vector HostCC registers
sudo cat /sys/kernel/tracing/trace_pipe          # bpf_printk output
```
The programs define `BPF_NO_KFUNC_PROTOTYPES` and declare each kfunc as `extern ... __ksym`. They include the committed `tools/testing/selftests/bpf/include/vmlinux.h`. Regenerate that header after kernel changes with `tools/bpf/bpftool/bpftool btf dump file vmlinux format c > ...`.

## tg3 kfunc architecture

The code is at the bottom of `tg3.c`, under `#ifdef CONFIG_BPF_SYSCALL`. The shared kfunc rules below also apply to the bnxt version:
- **Module init:** `module_pci_driver()` was replaced with an explicit `tg3_module_init()`. It calls `register_btf_kfunc_id_set(BPF_PROG_TYPE_SCHED_CLS, ...)` before `pci_register_driver`. The kfuncs are registered for TC only.
- **Arguments:** the netdev is passed as a scalar `ifindex`, because the verifier rejects `struct net_device *` as kfunc arg0. The kfunc resolves it with `dev_get_by_index_rcu(&init_net, ...)` inside `rcu_read_lock()`. It then checks `dev->netdev_ops == &tg3_netdev_ops` so that only this driver's devices are accepted.
- **Validation:** the kfunc checks the same limits as `tg3_set_coalesce` (`MAX_RXCOL_TICKS`, `MAX_RXMAX_FRAMES`, usecs != 0). It returns `-EAGAIN` during `tp->pcierr_recovery`.
- **Hardware writes:** they are plain MMIO (`tw32`) under `tg3_full_lock` (`spin_lock_bh(&tp->lock)`), so they are safe from TC/softirq context. bnxt is different: its coalescing goes through sleeping HWRM firmware calls and must be deferred (see `bnxt/CLAUDE.md`).

tg3 coalescing data flow:
- `tp->coal` (`struct ethtool_coalesce`) is the single global cache. `tg3_set_coalesce` (ethtool -C) and `tg3_reset_hw` (link up, reset) program the hardware from it via `__tg3_set_coalesce` → `tg3_coal_tx_init` / `tg3_coal_rx_init`.
- `tg3_write_rx_coalesce_vector(tp, vec, ...)` writes one RSS HostCC vector at `HOSTCC_*_VEC1 + vec * 0x18`. It was factored out of `tg3_coal_rx_init` for the per-queue kfunc.
- `bpf_tg3_set_rx_coalesce` updates `tp->coal` and reprograms all RX vectors, so the change persists across resets.
- `bpf_tg3_set_rx_coalesce_queue` writes hardware only. It needs `ENABLE_RSS` and `queue < tp->rxq_cnt`. Any later ethtool -C or link down/up overwrites it from `tp->coal`.
- Queue mapping with MSI-X and RSS: Linux RX queue `q` = HostCC vector `q` = `tp->napi[q + 1]`. `napi[0]` handles link and status only and has no RX ring. With RSS off, the non-VEC `HOSTCC_RX*` registers are used instead.
