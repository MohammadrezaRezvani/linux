# tg3 RX coalescing BPF kfunc experiment

Experimental restricted BPF kfuncs that let TC (`BPF_PROG_TYPE_SCHED_CLS`) programs
adjust RX interrupt coalescing on Broadcom Tigon3 (`tg3`) netdevs. The kfuncs call
existing driver helpers and program HostCC coalescing registers only — no generic
MMIO, no arbitrary register offsets.

**Status:** local research prototype (BCM5720 / `eno3` verified). Not intended for
upstream submission as-is.

## Goal

Incrementally prove: eBPF → kfunc → tg3 driver → HostCC registers, with verifier-safe
arguments and driver-side validation.

## Kernel configuration

```
CONFIG_TIGON3=m
CONFIG_BPF_SYSCALL=y
CONFIG_DEBUG_INFO_BTF_MODULES=y
```

Build the module:

```bash
cd /path/to/linux
make -j$(nproc) M=drivers/net/ethernet/broadcom modules
sudo rmmod tg3 2>/dev/null; sudo insmod drivers/net/ethernet/broadcom/tg3.ko
```

## Architecture

```
TC BPF program (egress clsact)
        │
        ▼
bpf_tg3_set_rx_coalesce[_queue]()   [tg3.ko kfunc, SCHED_CLS only]
        │
        ├─ dev_get_by_index_rcu(ifindex)
        ├─ netdev_ops == &tg3_netdev_ops
        └─ tg3_full_lock → HostCC programming → tg3_full_unlock
```

### Driver changes (`drivers/net/ethernet/broadcom/tg3.c`)

1. **`tg3_write_rx_coalesce_vector()`** — static helper used by `tg3_coal_rx_init()`
   and the per-queue kfunc. Programs one RSS HostCC vector:

   - `HOSTCC_RXCOL_TICKS_VEC1 + vector * 0x18`
   - `HOSTCC_RXMAX_FRAMES_VEC1 + vector * 0x18`
   - `HOSTCC_RXCOAL_MAXF_INT_VEC1 + vector * 0x18`

2. **`bpf_tg3_set_rx_coalesce()`** — global RX coalescing for all RSS vectors.
   Updates `tp->coal` and calls `tg3_coal_rx_init()` when the interface is running.

3. **`bpf_tg3_set_rx_coalesce_queue()`** — per Linux RX queue / HostCC vector.
   Calls `tg3_write_rx_coalesce_vector()` only. Does **not** update `tp->coal`.

Kfuncs are registered at module init via `register_btf_kfunc_id_set()` for
`BPF_PROG_TYPE_SCHED_CLS` only.

## Kfunc API

### `bpf_tg3_set_rx_coalesce(ifindex, rx_usecs, rx_frames)`

| Argument     | Description                                      |
|--------------|--------------------------------------------------|
| `ifindex`    | Netdev ifindex (scalar; verifier-safe)           |
| `rx_usecs`   | `1 .. 0x3ff` (`MAX_RXCOL_TICKS`)                 |
| `rx_frames`  | `0 .. 0xff` (`MAX_RXMAX_FRAMES`)                 |

**Returns:** `0` on success; `-EINVAL`, `-EAGAIN` (PCI error recovery), etc.

When RSS is enabled, programs every active vector (`0 .. rxq_cnt-1`) with the same
values. When the interface is down, only `tp->coal` is updated; hardware is
programmed on the next open/reset.

### `bpf_tg3_set_rx_coalesce_queue(ifindex, queue, rx_usecs, rx_frames)`

| Argument     | Description                                      |
|--------------|--------------------------------------------------|
| `queue`      | Linux RX queue index `0 .. rxq_cnt-1`            |
| (others)     | Same as global kfunc                             |

**Returns:** `-EOPNOTSUPP` if RSS is off; `-EINVAL` if `queue >= rxq_cnt`.

Programs a single HostCC vector. `tp->coal` is unchanged — `ethtool -c` still
reports the global cache; `ethtool -C` or `ip link down; up` reprograms all
vectors from `tp->coal` and overwrites per-queue BPF settings.

## Per-queue / vector mapping (RSS / MSI-X)

On BCM5720 with MSI-X and RSS (`ENABLE_RSS`, typical multiqueue setup):

| Linux RX queue | `napi[]` index | HostCC VEC | `RXCOL_TICKS` reg | `RXMAX_FRAMES` reg |
|----------------|----------------|------------|-------------------|--------------------|
| 0              | 1              | 0          | `0x3d80`          | `0x3d88`           |
| 1              | 2              | 1          | `0x3d98`          | `0x3da0`           |
| 2              | 3              | 2          | `0x3db0`          | `0x3db8`           |
| 3              | 4              | 3          | `0x3dc8`          | `0x3dd0`           |

`napi[0]` handles link/status only (no RX completion ring, no VEC coalesce block).

**Rule:** kfunc `queue` = Linux RX queue = HostCC vector index = `napi_index - 1`.

Hardware limits on 5720: `rxq_max = 4`, `irq_max = 5` (4 RSS + 1 status vector).

## BPF selftests

Sources (not wired into `run_tests.sh` yet):

- `tools/testing/selftests/bpf/progs/tg3_coal_test.c` — global 100/20
- `tools/testing/selftests/bpf/progs/tg3_coal_queue_test.c` — queue 0 → 80/15,
  queue 2 → 120/25

### Generate `vmlinux.h` (one-time per kernel build)

```bash
tools/bpf/bpftool/bpftool btf dump file vmlinux format c \
  > tools/testing/selftests/bpf/include/vmlinux.h
```

### Compile BPF objects

```bash
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 \
  -I tools/testing/selftests/bpf/include -I tools/include/uapi \
  -c tools/testing/selftests/bpf/progs/tg3_coal_test.c \
  -o tg3_coal_test.o -mcpu=v3

clang -g -O2 -target bpf -D__TARGET_ARCH_x86 \
  -I tools/testing/selftests/bpf/include -I tools/include/uapi \
  -c tools/testing/selftests/bpf/progs/tg3_coal_queue_test.c \
  -o tg3_coal_queue_test.o -mcpu=v3
```

### Attach and trigger

Egress clsact works without carrier (NO-CARRIER is fine for triggering).

```bash
IF=eno3
sudo ip link set $IF up
sudo tc qdisc add dev $IF clsact

# Global test:
sudo tc filter add dev $IF egress bpf da obj tg3_coal_test.o sec tc

# Per-queue test:
sudo tc filter add dev $IF egress bpf da obj tg3_coal_queue_test.o sec tc

ping -c 3 -I $IF 192.0.2.1
```

**Note:** `SEC("tc")` means the tc section name is `tc`, not the C function name.

### Verify

```bash
ethtool -c $IF                              # global cache (tp->coal)
ethtool -l $IF                              # rxq_cnt
sudo ethtool -d $IF | rg '0x3d[89ab][0-9a-f]'   # hardware registers
```

**Per-queue test expected dump** (after queue test, with q1/q3 at prior defaults):

| Queue | Ticks  | Frames |
|-------|--------|--------|
| 0     | `0x50` | `0x0f` |
| 1     | (unchanged) | (unchanged) |
| 2     | `0x78` | `0x19` |

## Verified behavior (BCM5720 / eno3)

| Step | Result |
|------|--------|
| Global BPF 100/20 | `ethtool -c` and `ethtool -d` both 100/20 |
| `ip link down` | `ethtool -d` empty (low power); cache persists |
| `ip link up` | Hardware restored from `tp->coal` via reset |
| `ethtool -C 50/10` | Both paths show 50/10; ethtool still works |
| Per-queue BPF | q0/q2 diverge; q1/q3 unchanged in hardware |

## Limitations

- **TC-only:** kfuncs registered for `BPF_PROG_TYPE_SCHED_CLS` only.
- **RX-only:** TX coalescing and stats-block coalescing are not exposed.
- **tg3-only:** `netdev_ops` must be `&tg3_netdev_ops`; no cross-driver access.
- **No kfunc unregister:** `rmmod tg3` fails while BPF programs reference kfuncs.
- **Per-queue cache:** `tp->coal` is global; per-queue kfunc is hardware-only.
- **`ethtool -d` when down:** register dump unavailable in low-power state.
- **`ifindex` only:** `struct net_device *` from BPF is rejected by the verifier
  for kfunc arg0; driver resolves netdev under RCU.

## Files

| File | Purpose |
|------|---------|
| `drivers/net/ethernet/broadcom/tg3.c` | helper, kfuncs, BTF registration |
| `tools/testing/selftests/bpf/progs/tg3_coal_test.c` | global smoke test |
| `tools/testing/selftests/bpf/progs/tg3_coal_queue_test.c` | per-queue smoke test |
| `tools/testing/selftests/bpf/include/vmlinux.h` | generated BTF header (local) |
