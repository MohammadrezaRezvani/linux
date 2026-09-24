// SPDX-License-Identifier: GPL-2.0
/*
 * cpufreq_ext_coal.c - cpufreq ext governor that pairs each frequency
 * decision with NIC RX interrupt coalescing (rx-usecs).
 *
 * coal_cfg[0].mode picks a joint operating point:
 *   0 = performance: policy->max,  "fast" coalescing (low latency)
 *   1 = powersave:   policy->min,  "slow" coalescing (fewer interrupts)
 *
 * get_next_freq() runs from the dbs workqueue with policy_dbs->update_mutex
 * held (process context). By default it uses the non-sleeping
 * bpf_netdev_request_rx_coalesce(). Built with -DCOAL_SYNC, get_next_freq is
 * a sleepable struct_ops program and calls bpf_netdev_set_rx_coalesce(),
 * which applies synchronously and returns the driver's result (it blocks the
 * frequency update while it waits for rtnl and the NIC).
 *
 * Only the policy that contains coal_cfg.decider_cpu makes coalescing
 * requests, at most once per interval_ns. Coalescing is off while
 * coal_cfg.ifindex is 0.
 *
 * Needs "ext" as scaling_governor before registering:
 *   echo ext > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
 *   bpftool struct_ops register cpufreq_ext_coal.o /sys/fs/bpf/cpufreq_coal
 *   (the link must be pinned or the governor unregisters when bpftool exits;
 *   rm -r /sys/fs/bpf/cpufreq_coal unregisters it)
 */
#define BPF_NO_KFUNC_PROTOTYPES
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

extern int bpf_netdev_request_rx_coalesce(__u32 ifindex, __u32 rx_usecs) __ksym;
extern int bpf_netdev_set_rx_coalesce(__u32 ifindex, __u32 rx_usecs) __ksym __weak;

struct coal_cfg {
	__u32 ifindex;		/* 0 = don't touch coalescing */
	__u32 decider_cpu;
	__u32 mode;		/* 0 = performance, 1 = powersave */
	__u32 fast_usecs;
	__u32 slow_usecs;
	__u32 pad;
	__u64 interval_ns;	/* minimum time between coalescing requests */
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct coal_cfg);
} coal_cfg SEC(".maps");

/* Observability: bpftool map dump name coal_stats */
struct coal_stats {
	__u64 last_req_ns;
	__u64 calls;		/* get_next_freq invocations */
	__u64 requests;		/* coalescing kfunc calls */
	__s64 last_rc;
	__u64 last_freq;
	__u32 last_mode;
	__u32 last_usecs;
	__u32 sync;		/* 1 if built with COAL_SYNC */
	__u32 pad;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct coal_stats);
} coal_stats SEC(".maps");

static bool policy_has_cpu(struct cpufreq_policy *policy, u32 cpu)
{
	/* cpufreq policies on x86 are usually per-CPU; policy->cpu is enough */
	return policy->cpu == cpu;
}

#ifdef COAL_SYNC
SEC("struct_ops.s/get_next_freq")
#else
SEC("struct_ops/get_next_freq")
#endif
unsigned long BPF_PROG(bpf_cpufreq_get_next_freq, struct cpufreq_policy *policy)
{
	struct coal_stats *st;
	struct coal_cfg *cfg;
	unsigned long freq;
	u32 key = 0, usecs;
	u64 now;

	cfg = bpf_map_lookup_elem(&coal_cfg, &key);
	st = bpf_map_lookup_elem(&coal_stats, &key);
	if (!cfg || !st)
		return policy->max;

	freq = cfg->mode ? policy->min : policy->max;
	__sync_fetch_and_add(&st->calls, 1);

	if (!cfg->ifindex || !policy_has_cpu(policy, cfg->decider_cpu))
		return freq;

	now = bpf_ktime_get_ns();
	if (now - st->last_req_ns < cfg->interval_ns)
		return freq;
	st->last_req_ns = now;

	usecs = cfg->mode ? cfg->slow_usecs : cfg->fast_usecs;

#ifdef COAL_SYNC
	st->last_rc = bpf_netdev_set_rx_coalesce(cfg->ifindex, usecs);
	st->sync = 1;
#else
	st->last_rc = bpf_netdev_request_rx_coalesce(cfg->ifindex, usecs);
	st->sync = 0;
#endif
	st->requests++;
	st->last_freq = freq;
	st->last_mode = cfg->mode;
	st->last_usecs = usecs;
	return freq;
}

SEC("struct_ops/get_sampling_rate")
unsigned int BPF_PROG(bpf_cpufreq_get_sampling_rate, struct cpufreq_policy *policy)
{
	return 0;	/* keep the dbs default */
}

SEC("struct_ops.s/init")
unsigned int BPF_PROG(bpf_cpufreq_init)
{
	return 0;
}

SEC("struct_ops.s/exit")
void BPF_PROG(bpf_cpufreq_exit)
{
}

SEC(".struct_ops.link")
struct cpufreq_governor_ext_ops cpufreq_coal_ops = {
	.get_next_freq		= (void *)bpf_cpufreq_get_next_freq,
	.get_sampling_rate	= (void *)bpf_cpufreq_get_sampling_rate,
	.init			= (void *)bpf_cpufreq_init,
	.exit			= (void *)bpf_cpufreq_exit,
	.name			= "BPF_cpufreq_coal",
};
