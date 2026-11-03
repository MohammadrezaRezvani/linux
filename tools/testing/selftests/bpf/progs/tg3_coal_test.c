// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal TC egress smoke test for bpf_tg3_set_rx_coalesce() (tg3 driver kfunc).
 *
 * Attach: sudo tc filter add dev <if> egress bpf da obj tg3_coal_test.o sec tc
 */
#define BPF_NO_KFUNC_PROTOTYPES
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#ifndef TC_ACT_OK
#define TC_ACT_OK 0
#endif

extern int bpf_tg3_set_rx_coalesce(__u32 ifindex, __u32 rx_usecs,
				   __u32 rx_frames) __ksym;

SEC("tc")
int tg3_coal_test(struct __sk_buff *skb)
{
	int err;

	err = bpf_tg3_set_rx_coalesce(skb->ifindex, 100, 20);
	bpf_printk("tg3_coal_test: bpf_tg3_set_rx_coalesce returned %d\n", err);

	return TC_ACT_OK;
}

char _license[] SEC("license") = "GPL";
