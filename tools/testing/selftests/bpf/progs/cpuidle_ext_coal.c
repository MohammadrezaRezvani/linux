// SPDX-License-Identifier: GPL-2.0
/*
 * cpuidle_ext_coal.c - cpuidle ext governor that also steers NIC RX
 * interrupt coalescing through bpf_netdev_request_rx_coalesce().
 *
 * Idle-state selection is the same as cpuidle_gov_ext.c. In addition, on one
 * "decider" CPU and at most once per interval, the governor asks for the
 * "deep" rx-usecs when it picked the deepest enabled idle state and the
 * "shallow" rx-usecs otherwise. select() runs with IRQs disabled, so it must
 * use the non-sleeping request kfunc; the kernel applies the change from a
 * worker and drops requests that don't change anything.
 *
 * Coalescing is off until coal_cfg[0].ifindex is set, e.g.:
 *   bpftool struct_ops register cpuidle_ext_coal.o /sys/fs/bpf/cpuidle_coal
 *   bpftool map update name coal_cfg key 0 0 0 0 value <struct bytes>
 */
#define BPF_NO_KFUNC_PROTOTYPES
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#define ALPHA 10
#define ALPHA_SCALE 100
#define FIT_FACTOR 90

int bpf_cpuidle_ext_gov_update_rating(unsigned int rating) __ksym __weak;
s64 bpf_cpuidle_ext_gov_latency_req(unsigned int cpu) __ksym __weak;
s64 bpf_tick_nohz_get_sleep_length(void) __ksym __weak;
extern int bpf_netdev_request_rx_coalesce(__u32 ifindex, __u32 rx_usecs) __ksym;

struct coal_cfg {
	__u32 ifindex;		/* 0 = don't touch coalescing */
	__u32 decider_cpu;
	__u32 shallow_usecs;
	__u32 deep_usecs;
	__u64 interval_ns;	/* minimum time between requests */
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
	__u64 requests;
	__u64 deep_requests;
	__s64 last_rc;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct coal_stats);
} coal_stats SEC(".maps");

struct cpuidle_gov_data {
	int cpu;
	int last_idx;
	u64 last_pred;
	u64 last_duration;
	u64 next_pred;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct cpuidle_gov_data);
} cpuidle_gov_data_map SEC(".maps");

static u64 calculate_ewma(u64 last, u64 new, u32 alpha, u32 alpha_scale)
{
	return (alpha * new + (alpha_scale - alpha) * last) / alpha_scale;
}

static void update_predict_duration(struct cpuidle_gov_data *data,
				    struct cpuidle_driver *drv,
				    struct cpuidle_device *dev)
{
	struct cpuidle_state target;
	int idx;

	idx = data->last_idx;
	data->last_duration = dev->last_residency_ns;
	if (idx > 0) {
		bpf_core_read(&target, sizeof(target), &drv->states[idx]);
		if (data->last_duration > target.exit_latency)
			data->last_duration -= target.exit_latency;
	}
	data->last_pred = data->next_pred;
	data->next_pred = calculate_ewma(data->next_pred,
					 data->last_duration, ALPHA, ALPHA_SCALE);
}

static void steer_coalescing(struct cpuidle_device *dev, bool deep)
{
	struct coal_stats *st;
	struct coal_cfg *cfg;
	u32 key = 0;
	u64 now;

	cfg = bpf_map_lookup_elem(&coal_cfg, &key);
	if (!cfg || !cfg->ifindex || dev->cpu != cfg->decider_cpu)
		return;
	st = bpf_map_lookup_elem(&coal_stats, &key);
	if (!st)
		return;

	now = bpf_ktime_get_ns();
	if (now - st->last_req_ns < cfg->interval_ns)
		return;
	st->last_req_ns = now;

	if (deep)
		st->last_rc = bpf_netdev_request_rx_coalesce(cfg->ifindex,
							     cfg->deep_usecs);
	else
		st->last_rc = bpf_netdev_request_rx_coalesce(cfg->ifindex,
							     cfg->shallow_usecs);
	st->requests++;
	if (deep)
		st->deep_requests++;
}

SEC("struct_ops.s/enable")
int BPF_PROG(bpf_cpuidle_enable, struct cpuidle_driver *drv,
	     struct cpuidle_device *dev)
{
	struct cpuidle_gov_data *data;
	u32 key = 0;

	data = bpf_map_lookup_percpu_elem(&cpuidle_gov_data_map, &key, dev->cpu);
	if (!data)
		return 0;

	__builtin_memset(data, 0, sizeof(struct cpuidle_gov_data));
	data->cpu = dev->cpu;
	return 0;
}

SEC("struct_ops.s/disable")
void BPF_PROG(bpf_cpuidle_disable, struct cpuidle_driver *drv,
	      struct cpuidle_device *dev)
{
}

/* Runs with IRQs disabled: must not be sleepable (see ops_check_member()). */
SEC("struct_ops/select")
int BPF_PROG(bpf_cpuidle_select, struct cpuidle_driver *drv,
	     struct cpuidle_device *dev)
{
	unsigned long long disable = 0;
	struct cpuidle_gov_data *data;
	s64 delta, latency_req;
	struct cpuidle_state cs;
	u32 key = 0;
	int i;

	data = bpf_map_lookup_percpu_elem(&cpuidle_gov_data_map, &key, dev->cpu);
	if (!data)
		return 0;
	latency_req = bpf_cpuidle_ext_gov_latency_req(dev->cpu);
	delta = bpf_tick_nohz_get_sleep_length();

	update_predict_duration(data, drv, dev);

	for (i = ARRAY_SIZE(drv->states) - 1; i > 0; i--) {
		if (i > drv->state_count - 1)
			continue;
		bpf_core_read(&cs, sizeof(cs), &drv->states[i]);
		bpf_core_read(&disable, sizeof(disable), &dev->states_usage[i]);

		if (disable)
			continue;
		if (latency_req < cs.exit_latency_ns)
			continue;
		if (delta < cs.target_residency_ns)
			continue;
		if (data->next_pred / FIT_FACTOR * ALPHA_SCALE < cs.target_residency_ns)
			continue;
		break;
	}

	steer_coalescing(dev, i > 0 && i == drv->state_count - 1);
	return i;
}

SEC("struct_ops/set_stop_tick")
bool BPF_PROG(bpf_cpuidle_set_stop_tick)
{
	return false;
}

SEC("struct_ops/reflect")
void BPF_PROG(bpf_cpuidle_reflect, struct cpuidle_device *dev, int index)
{
	struct cpuidle_gov_data *data;
	u32 key = 0;

	data = bpf_map_lookup_percpu_elem(&cpuidle_gov_data_map, &key, dev->cpu);
	if (!data)
		return;
	data->last_idx = index;
}

SEC("struct_ops.s/init")
int BPF_PROG(bpf_cpuidle_init)
{
	return bpf_cpuidle_ext_gov_update_rating(60);
}

SEC("struct_ops.s/exit")
void BPF_PROG(bpf_cpuidle_exit)
{
}

SEC(".struct_ops.link")
struct cpuidle_gov_ext_ops ops = {
	.enable		= (void *)bpf_cpuidle_enable,
	.disable	= (void *)bpf_cpuidle_disable,
	.select		= (void *)bpf_cpuidle_select,
	.set_stop_tick	= (void *)bpf_cpuidle_set_stop_tick,
	.reflect	= (void *)bpf_cpuidle_reflect,
	.init		= (void *)bpf_cpuidle_init,
	.exit		= (void *)bpf_cpuidle_exit,
	.name		= "BPF_cpuidle_coal"
};
