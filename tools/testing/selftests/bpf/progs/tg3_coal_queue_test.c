// SPDX-License-Identifier: GPL-2.0
/*
 * TC egress smoke test for bpf_tg3_set_rx_coalesce_queue() (tg3 driver kfunc).
 *
 * Programs queue 0 to 80/15 and queue 2 to 120/25; other queues are untouched.
 * Attach (section is SEC("tc"), not the function name):
 *   sudo tc filter add dev <if> egress bpf da obj tg3_coal_queue_test.o sec tc
 * After triggering, verify with:
 *   sudo ethtool -d <if> | rg '0x3d[89ab][0-9a-f]'
 * Expect:
 *   0x3d80/0x3d88 = 0x50/0x0f (q0)
 *   0x3d98/0x3da0 = unchanged (q1)
 *   0x3db0/0x3db8 = 0x78/0x19 (q2)
 */
#define BPF_NO_KFUNC_PROTOTYPES
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#ifndef TC_ACT_OK
#define TC_ACT_OK 0
#endif

extern int bpf_tg3_set_rx_coalesce_queue(__u32 ifindex, __u32 queue,
					 __u32 rx_usecs,
					 __u32 rx_frames) __ksym;

SEC("tc")
int tg3_coal_queue_test(struct __sk_buff *skb)
{
	int err;

	err = bpf_tg3_set_rx_coalesce_queue(skb->ifindex, 0, 80, 15);
	if (err)
		bpf_printk("tg3_coal_queue_test: queue 0 failed %d\n", err);

	err = bpf_tg3_set_rx_coalesce_queue(skb->ifindex, 2, 120, 25);
	if (err)
		bpf_printk("tg3_coal_queue_test: queue 2 failed %d\n", err);

	return TC_ACT_OK;
}

char _license[] SEC("license") = "GPL";
