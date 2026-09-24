// SPDX-License-Identifier: GPL-2.0
/*
 * Drive the netdev RX coalescing kfuncs (net/ethtool/bpf_coalesce.c) from a
 * sleepable syscall program, without any governor loaded.
 *
 * Load with bpftool and run with netdev_coal/netdev_coal_run.py (bpftool prog
 * run can't run syscall programs). The context is struct netdev_coal_args,
 * and results are written back into it:
 *   bpftool prog load netdev_coal_syscall.o /sys/fs/bpf/netdev_coal
 *
 * mode 0: bpf_netdev_set_rx_coalesce()     (synchronous)
 * mode 1: bpf_netdev_request_rx_coalesce() (queued)
 * mode 2: bpf_netdev_get_rx_coalesce()     (read back)
 */
#define BPF_NO_KFUNC_PROTOTYPES
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

extern int bpf_netdev_set_rx_coalesce(__u32 ifindex, __u32 rx_usecs) __ksym;
extern int bpf_netdev_request_rx_coalesce(__u32 ifindex, __u32 rx_usecs) __ksym;
extern int bpf_netdev_get_rx_coalesce(__u32 ifindex, __u32 *rx_usecs) __ksym;

struct netdev_coal_args {
	__u32 ifindex;
	__u32 rx_usecs;
	__u32 mode;
	__s32 ret;
	__u32 got_usecs;
};

SEC("syscall")
int netdev_coal(struct netdev_coal_args *args)
{
	__u32 usecs = 0;

	switch (args->mode) {
	case 0:
		args->ret = bpf_netdev_set_rx_coalesce(args->ifindex,
						       args->rx_usecs);
		break;
	case 1:
		args->ret = bpf_netdev_request_rx_coalesce(args->ifindex,
							   args->rx_usecs);
		break;
	default:
		args->ret = bpf_netdev_get_rx_coalesce(args->ifindex, &usecs);
		args->got_usecs = usecs;
		break;
	}
	return 0;
}

char _license[] SEC("license") = "GPL";
