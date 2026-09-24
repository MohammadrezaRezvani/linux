# netdev RX coalescing BPF kfuncs

Driver-independent kfuncs that let BPF programs outside the networking stack,
mainly the `cpuidle_ext` and `cpufreq_ext` struct_ops governors, change a NIC's
RX interrupt coalescing timer (`ethtool -C <dev> rx-usecs U`).

**Status:** local research prototype. Source: `net/ethtool/bpf_coalesce.c`.

## Why the kfuncs are in net/ethtool, not in a driver

Drivers apply coalescing in different ways:

- **tg3:** writes HostCC registers directly (plain MMIO under a spinlock).
- **bnxt:** sends one `HWRM_RING_CMPL_RING_CFG_AGGINT_PARAMS` firmware message per
  completion ring. Each send takes `bp->hwrm_cmd_lock` (a mutex), allocates with
  `GFP_KERNEL`, and sleeps polling for the reply (`usleep_range`). A change takes
  about 2 ms on a BCM57412 with 16 rings.

Every driver already implements `ethtool_ops->set_coalesce`. It runs under rtnl
and the netdev instance lock, and it may sleep. The kfuncs call that same entry
point the same way ethtool netlink does (`ethnl_default_set_doit` +
`__ethnl_set_coalesce`), so:

- they work on any NIC with no driver changes;
- the driver's own validation applies;
- `ethtool -c` reports the new values, and `ethtool --monitor` sees a
  `COALESCE_NTF` notification.

## API

```c
int bpf_netdev_request_rx_coalesce(u32 ifindex, u32 rx_usecs);
int bpf_netdev_set_rx_coalesce(u32 ifindex, u32 rx_usecs);    /* KF_SLEEPABLE */
int bpf_netdev_get_rx_coalesce(u32 ifindex, u32 *rx_usecs);
```

They are registered for `STRUCT_OPS` (both governors), `SYSCALL`, `TRACING` and
`SCHED_CLS` programs. `ifindex` refers to a device in `init_net`, and
`rx_usecs` has its ethtool meaning (0 is passed to the driver like ethtool does;
bnxt accepts it, tg3 rejects it). The netdev only needs to support
`ETHTOOL_COALESCE_RX_USECS`.

| kfunc | Sleeps? | Use from | Returns |
|---|---|---|---|
| `request` | no, safe with IRQs off | cpuidle `select`/`reflect`, cpufreq `get_next_freq`, TC, anything | `0` queued / already in effect; `-EINVAL`, `-ENODEV`, `-EOPNOTSUPP`, `-ENOSPC` |
| `set` | yes | sleepable programs only: `struct_ops.s/*` in process context, `SEC("syscall")` | `0` or the driver's error; `-EBUSY` if adaptive RX is on |
| `get` | no | anything | `0` + current rx-usecs; `-EINPROGRESS`, `-ENOENT`, or the last driver error |

### Semantics

- **Only rx-usecs changes**, exactly as with `ethtool -C <dev> rx-usecs U`. The
  current settings are read with `get_coalesce()` and passed back unchanged. This
  preserves rx-frames, CQE mode and TX settings. It also keeps
  `stats-block-usecs` unchanged, which matters because
  bnxt closes and reopens the NIC when that value changes.
- **Adaptive RX (DIM):** if it is enabled, the kfuncs return `-EBUSY`, because DIM
  would overwrite the change on the next sample.
- **The request path** keeps a table of 8 netdev slots under a raw spinlock.
  - A request equal to the pending value or to the device's current rx-usecs
    returns 0 without waking the worker, so a governor can call it on every idle
    entry.
  - If a request is still pending, a new one replaces its values (last one wins).
  - A single work item on `system_unbound_wq` applies all pending slots.
  - Driver errors are only visible through `bpf_netdev_get_rx_coalesce()`.
- **Changes from outside BPF are tracked.** Every successful coalescing change,
  from these kfuncs or from `ethtool -C` (netlink or ioctl), sends
  `ETHTOOL_MSG_COALESCE_NTF` through `ethnl_notify()` under rtnl. That calls
  `netdev_coal_sync()`, which rereads rx-usecs from the driver for netdevs BPF
  has touched. So `get` reports the device's current rx-usecs, and the duplicate
  check never drops a request that ethtool has undone.
- **`get`** returns `-ENOENT` until BPF has requested or set a value on the netdev.

## Where each variant can run

| Caller | Context | Kfunc |
|---|---|---|
| cpuidle_ext `select`, `reflect`, `set_stop_tick` | idle loop, IRQs off | `request` only. These members must not be sleepable; `ops_check_member()` rejects `struct_ops.s/` for them |
| cpuidle_ext `enable`, `disable`, `init`, `exit` | process (`cpuidle_pause_and_lock` / registration) | either |
| cpufreq_ext `get_next_freq`, `get_sampling_rate` | dbs workqueue holding `policy_dbs->update_mutex` | prefer `request`. `set` works if the program is `struct_ops.s/`, but it holds up the frequency update while it waits for rtnl and the driver |

## Test programs

- `tools/testing/selftests/bpf/progs/netdev_coal_syscall.c`: a `SEC("syscall")`
  program that calls any of the three kfuncs with arguments from its context, so
  no governor is needed. `bpftool prog run` can't run syscall programs (the
  kernel rejects its `repeat=1` and `ctx_out` with `-EINVAL`), so
  `netdev_coal/netdev_coal_run.py` calls `BPF_PROG_TEST_RUN` directly and reads
  the results back from `ctx_in`.
- `tools/testing/selftests/bpf/progs/cpuidle_ext_coal.c`: the `cpuidle_gov_ext.c`
  policy plus coalescing steering. On `coal_cfg.decider_cpu`, at most once per
  `interval_ns`, it requests the "deep" rx-usecs when `select` picked the deepest
  enabled state and the "shallow" rx-usecs otherwise. It does nothing until
  `coal_cfg.ifindex` is set with `bpftool map update name coal_cfg ...`.
  Counters are in the `coal_stats` map.
- `tools/testing/selftests/bpf/progs/cpufreq_ext_coal.c`: a cpufreq ext governor
  that sets frequency and coalescing together. `coal_cfg.mode` 0 picks
  `policy->max` with the "fast" rx-usecs, and mode 1 picks `policy->min` with the
  "slow" rx-usecs. Requests come from the policy containing `coal_cfg.decider_cpu`,
  at most once per `interval_ns`. There are two builds of the same source:
  - the default build: `struct_ops/get_next_freq`, calls `request`;
  - `-DCOAL_SYNC`: `struct_ops.s/get_next_freq`, calls the sleepable `set`
    directly from the dbs worker. This exercises the synchronous path from a real
    governor callback.

  The `ext` governor must be the policy's `scaling_governor` before the program
  is registered (`ext_reg()` returns `-EEXIST` otherwise).

Build them with `tools/testing/selftests/bpf/netdev_coal/build.sh`. It uses the
libbpf headers generated by the bpftool build (`make -C tools/bpf/bpftool`).

Tests in `tools/testing/selftests/bpf/netdev_coal/` (run as root):

- `verify_netdev_coal.sh`: set/request/get behaviour, duplicate handling,
  tracking of `ethtool -C` changes, and error cases.
- `ethtool_equiv.sh`: for each rx-usecs value, compares `ethtool -C` with `set`
  and `request`: full `ethtool -c` output, every firmware
  `HWRM_RING_CMPL_RING_CFG_AGGINT_PARAMS` message, and `COALESCE_NTF` count.
- `cpufreq_coal_test.sh [async|sync]`: the cpufreq governor end to end.

Registering a governor with `.struct_ops.link` requires pinning the link, or it
unregisters as soon as bpftool exits. Removing the pin unregisters it:

```bash
echo ext > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
tools/bpf/bpftool/bpftool struct_ops register cpufreq_ext_coal.o /sys/fs/bpf/cpufreq_coal
...
rm -r /sys/fs/bpf/cpufreq_coal
# the link is released from a workqueue: wait for it before switching governor
while tools/bpf/bpftool/bpftool struct_ops show name cpufreq_coal_op >/dev/null 2>&1; do sleep 0.1; done
echo schedutil > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
```

Before the `cpufreq_ext.c` fix, switching the governor away before the
deferred `ext_unreg()` ran oopsed in `ext_disable()` (NULL `ext_global_tuner`)
with `ext_mutex` held, and every later registration hung.

Use the in-tree `tools/bpf/bpftool/bpftool`: on this machine, `/usr/sbin/bpftool`
is the distro wrapper, and it refuses to run on a custom kernel.

The committed `vmlinux.h` doesn't need regenerating for these kfuncs, because the
programs declare them with `extern ... __ksym` under `BPF_NO_KFUNC_PROTOTYPES`.

## Limitations

- **Global settings only.** Neither tg3 nor bnxt implements
  `set_per_queue_coalesce`. For per-queue control on tg3 use
  `bpf_tg3_set_rx_coalesce_queue()` (SCHED_CLS only).
- **rx-usecs only.** rx-frames and TX coalescing are passed through unchanged. On pre-P5 bnxt chips each
  completion ring serves both RX and TX, so the RX values also govern TX
  completions on those rings.
- **init_net only**, at most 8 tracked netdevs. A slot is freed when its device
  disappears (`-ENODEV` on apply).
- `cpuidle_gov_ext.c` now uses non-sleepable sections for `select`, `reflect`
  and `set_stop_tick`, as the new `ops_check_member()` rule requires.
