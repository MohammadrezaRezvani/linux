// SPDX-License-Identifier: GPL-2.0-only
/*
 * BPF kfuncs to change RX interrupt coalescing (rx-usecs) on any netdev that implements
 * ethtool_ops->set_coalesce, from BPF programs outside the networking stack
 * (e.g. the cpuidle/cpufreq "ext" struct_ops governors).
 *
 * The driver call has to run in process context: it runs under rtnl and the
 * netdev instance lock, and drivers may sleep in it (bnxt sends one firmware
 * message per completion ring). Two entry points are provided:
 *
 *  - bpf_netdev_request_rx_coalesce(): never sleeps and is safe with IRQs
 *    disabled (cpuidle select()). Records the request and queues a worker.
 *  - bpf_netdev_set_rx_coalesce(): KF_SLEEPABLE, applies synchronously and
 *    returns the driver's result.
 *
 * Both go through netdev_apply_rx_coalesce(), which mirrors
 * "ethtool -C <dev> rx-usecs U" (the netlink COALESCE_SET path): only
 * rx-usecs changes, every other setting is read back from the driver and
 * passed through unchanged.
 *
 * Every successful coalescing change, whether from BPF or from ethtool
 * (netlink or ioctl), ends in ethnl_notify(ETHTOOL_MSG_COALESCE_NTF) under
 * rtnl, which calls netdev_coal_sync() to reread the driver's rx-usecs. So the
 * request path's duplicate check and bpf_netdev_get_rx_coalesce() follow
 * changes made outside BPF.
 */

#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>
#include <net/netdev_lock.h>

#include "netlink.h"

#define NETDEV_COAL_SLOTS	8

/*
 * One slot per netdev that BPF has touched. ifindex == 0 means free.
 * @known/@cur_usecs: the device's rx-usecs after the last coalescing change
 * (from BPF or ethtool), valid if @known.
 */
struct netdev_coal_slot {
	int	ifindex;
	u32	req_usecs;
	bool	pending;
	bool	known;
	u32	cur_usecs;
	int	last_rc;
};

static struct netdev_coal_slot netdev_coal_slots[NETDEV_COAL_SLOTS];
/* raw: taken with IRQs disabled from the cpuidle select() path */
static DEFINE_RAW_SPINLOCK(netdev_coal_lock);

static void netdev_coal_workfn(struct work_struct *work);
static DECLARE_WORK(netdev_coal_work, netdev_coal_workfn);

static bool netdev_coal_supported(const struct net_device *dev)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;

	return ops && ops->get_coalesce && ops->set_coalesce &&
	       (ops->supported_coalesce_params & ETHTOOL_COALESCE_RX_USECS);
}

/* Caller holds netdev_coal_lock. Returns NULL if the table is full. */
static struct netdev_coal_slot *netdev_coal_slot_get(int ifindex, bool alloc)
{
	struct netdev_coal_slot *free = NULL;
	int i;

	for (i = 0; i < NETDEV_COAL_SLOTS; i++) {
		struct netdev_coal_slot *s = &netdev_coal_slots[i];

		if (s->ifindex == ifindex)
			return s;
		if (!s->ifindex && !free)
			free = s;
	}
	if (!alloc || !free)
		return NULL;

	memset(free, 0, sizeof(*free));
	free->ifindex = ifindex;
	return free;
}

/*
 * Record the result of an apply. If @usecs_known, @usecs is the device's
 * rx-usecs. After a successful set_coalesce, netdev_coal_sync() has already
 * read it from the driver (which may round it).
 */
static void netdev_coal_record(int ifindex, int rc, bool usecs_known, u32 usecs)
{
	struct netdev_coal_slot *s;
	unsigned long flags;

	raw_spin_lock_irqsave(&netdev_coal_lock, flags);
	s = netdev_coal_slot_get(ifindex, rc != -ENODEV);
	if (s) {
		if (rc == -ENODEV) {
			/* device is gone: free the slot */
			s->ifindex = 0;
		} else {
			s->last_rc = rc;
			if (usecs_known) {
				s->known = true;
				s->cur_usecs = usecs;
			}
		}
	}
	raw_spin_unlock_irqrestore(&netdev_coal_lock, flags);
}

/**
 * netdev_coal_sync - Reread rx-usecs after a coalescing change
 * @dev: the netdev whose coalescing settings changed
 *
 * Called from ethnl_notify() for ETHTOOL_MSG_COALESCE_NTF, with rtnl and the
 * netdev instance lock held. Does nothing for netdevs BPF never touched.
 */
void netdev_coal_sync(struct net_device *dev)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct kernel_ethtool_coalesce kec = {};
	struct ethtool_coalesce ec = {};
	struct netdev_coal_slot *s;
	unsigned long flags;
	bool tracked;
	int ret;

	raw_spin_lock_irqsave(&netdev_coal_lock, flags);
	tracked = netdev_coal_slot_get(dev->ifindex, false);
	raw_spin_unlock_irqrestore(&netdev_coal_lock, flags);
	if (!tracked || !ops || !ops->get_coalesce)
		return;

	ret = ops->get_coalesce(dev, &ec, &kec, NULL);

	raw_spin_lock_irqsave(&netdev_coal_lock, flags);
	s = netdev_coal_slot_get(dev->ifindex, false);
	if (s) {
		s->known = ret >= 0;
		s->cur_usecs = ec.rx_coalesce_usecs;
		s->last_rc = 0;
	}
	raw_spin_unlock_irqrestore(&netdev_coal_lock, flags);
}

static int netdev_apply_rx_coalesce(struct net_device *dev, u32 usecs)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct kernel_ethtool_coalesce kec = {};
	struct ethtool_coalesce ec = {};
	bool known = false;
	unsigned long flags;
	int ret;

	if (!netdev_coal_supported(dev))
		return -EOPNOTSUPP;

	rtnl_lock();
	netdev_lock_ops(dev);
	/* track the device first so that netdev_coal_sync() sees the change */
	raw_spin_lock_irqsave(&netdev_coal_lock, flags);
	netdev_coal_slot_get(dev->ifindex, true);
	raw_spin_unlock_irqrestore(&netdev_coal_lock, flags);

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		goto out_unlock;

	/* Start from the current settings so that rx-frames, CQE mode,
	 * stats-block-usecs etc. are passed through unchanged (bnxt reopens
	 * the NIC when stats-block-usecs changes).
	 */
	ret = ops->get_coalesce(dev, &ec, &kec, NULL);
	if (ret < 0)
		goto out_complete;

	/* Adaptive (DIM) coalescing would overwrite the values right away. */
	if (ec.use_adaptive_rx_coalesce) {
		ret = -EBUSY;
		goto out_complete;
	}

	/* like ethtool, no driver call and no notification if nothing changes */
	if (ec.rx_coalesce_usecs == usecs) {
		known = true;
		ret = 0;
		goto out_complete;
	}

	ec.rx_coalesce_usecs = usecs;
	ret = ops->set_coalesce(dev, &ec, &kec, NULL);
	if (!ret)
		ethtool_notify(dev, ETHTOOL_MSG_COALESCE_NTF);

out_complete:
	ethnl_ops_complete(dev);
out_unlock:
	/* under rtnl, so a concurrent ethtool change can't be overwritten */
	netdev_coal_record(dev->ifindex, ret, known, usecs);
	netdev_unlock_ops(dev);
	rtnl_unlock();
	return ret;
}

static int netdev_coal_apply_ifindex(int ifindex, u32 usecs)
{
	struct net_device *dev;
	int rc;

	dev = dev_get_by_index(&init_net, ifindex);
	if (!dev) {
		netdev_coal_record(ifindex, -ENODEV, false, 0);
		return -ENODEV;
	}
	rc = netdev_apply_rx_coalesce(dev, usecs);
	dev_put(dev);
	return rc;
}

static void netdev_coal_workfn(struct work_struct *work)
{
	struct {
		int ifindex;
		u32 usecs;
	} todo[NETDEV_COAL_SLOTS];
	unsigned long flags;
	int i, n = 0;

	raw_spin_lock_irqsave(&netdev_coal_lock, flags);
	for (i = 0; i < NETDEV_COAL_SLOTS; i++) {
		struct netdev_coal_slot *s = &netdev_coal_slots[i];

		if (!s->ifindex || !s->pending)
			continue;
		s->pending = false;
		todo[n].ifindex = s->ifindex;
		todo[n].usecs = s->req_usecs;
		n++;
	}
	raw_spin_unlock_irqrestore(&netdev_coal_lock, flags);

	for (i = 0; i < n; i++) {
		int rc = netdev_coal_apply_ifindex(todo[i].ifindex,
						   todo[i].usecs);

		if (rc)
			net_dbg_ratelimited("netdev_coal: ifindex %d rx-usecs %u failed: %d\n",
					    todo[i].ifindex, todo[i].usecs, rc);
	}
}

__bpf_kfunc_start_defs();

/**
 * bpf_netdev_request_rx_coalesce - Queue an rx-usecs change
 * @ifindex: netdev ifindex in init_net
 * @rx_usecs: ethtool rx-usecs
 *
 * Never sleeps; safe with IRQs disabled. The change is applied shortly after
 * by a worker on system_unbound_wq, like "ethtool -C <dev> rx-usecs U". A
 * request equal to the pending value, or to the device's current rx-usecs
 * (including changes made with ethtool), is dropped without waking the worker.
 *
 * Return: 0 if queued (or already in effect), -EINVAL for ifindex 0,
 * -ENODEV if there is no such netdev, -EOPNOTSUPP if its driver can't set
 * rx-usecs, -ENOSPC if too many netdevs are tracked, -EBUSY in NMI context.
 * Errors from the driver are reported through bpf_netdev_get_rx_coalesce().
 */
__bpf_kfunc int bpf_netdev_request_rx_coalesce(u32 ifindex, u32 rx_usecs)
{
	struct netdev_coal_slot *s;
	struct net_device *dev;
	unsigned long flags;
	bool queue = true;
	int rc = 0;

	if (!ifindex || ifindex > INT_MAX)
		return -EINVAL;
	/* netdev_coal_lock may already be held on this CPU */
	if (in_nmi())
		return -EBUSY;

	rcu_read_lock();
	dev = dev_get_by_index_rcu(&init_net, ifindex);
	if (!dev)
		rc = -ENODEV;
	else if (!netdev_coal_supported(dev))
		rc = -EOPNOTSUPP;
	rcu_read_unlock();
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&netdev_coal_lock, flags);
	s = netdev_coal_slot_get(ifindex, true);
	if (!s) {
		rc = -ENOSPC;
		queue = false;
	} else if (s->pending) {
		/* worker hasn't picked it up yet: just update the value */
		s->req_usecs = rx_usecs;
	} else if (s->known && !s->last_rc && s->cur_usecs == rx_usecs) {
		queue = false;
	} else {
		s->req_usecs = rx_usecs;
		s->pending = true;
	}
	raw_spin_unlock_irqrestore(&netdev_coal_lock, flags);

	if (queue)
		queue_work(system_unbound_wq, &netdev_coal_work);
	return rc;
}

/**
 * bpf_netdev_set_rx_coalesce - Apply an rx-usecs change synchronously
 * @ifindex: netdev ifindex in init_net
 * @rx_usecs: ethtool rx-usecs
 *
 * Sleepable programs only. Takes rtnl and the netdev instance lock and calls
 * the driver's set_coalesce, like "ethtool -C <dev> rx-usecs U".
 *
 * Return: 0 on success, -ENODEV, -EOPNOTSUPP, -EBUSY if adaptive RX is
 * enabled, or the driver's error.
 */
__bpf_kfunc int bpf_netdev_set_rx_coalesce(u32 ifindex, u32 rx_usecs)
{
	if (!ifindex || ifindex > INT_MAX)
		return -EINVAL;

	return netdev_coal_apply_ifindex(ifindex, rx_usecs);
}

/**
 * bpf_netdev_get_rx_coalesce - Read the device's rx-usecs
 * @ifindex: netdev ifindex in init_net
 * @rx_usecs: filled with the rx-usecs after the last coalescing change
 *
 * Never sleeps. Only works for netdevs that BPF has already requested or set
 * a value on; from then on it follows every coalescing change, including
 * "ethtool -C".
 *
 * Return: 0 if @rx_usecs is valid and the last BPF attempt succeeded, the
 * last driver error if it failed, -EINPROGRESS if a request is still pending,
 * or -ENOENT if BPF never changed this netdev.
 */
__bpf_kfunc int bpf_netdev_get_rx_coalesce(u32 ifindex, u32 *rx_usecs)
{
	struct netdev_coal_slot *s;
	unsigned long flags;
	int rc;

	if (!ifindex || ifindex > INT_MAX)
		return -EINVAL;
	if (in_nmi())
		return -EBUSY;

	raw_spin_lock_irqsave(&netdev_coal_lock, flags);
	s = netdev_coal_slot_get(ifindex, false);
	if (!s) {
		rc = -ENOENT;
	} else {
		if (s->known)
			*rx_usecs = s->cur_usecs;
		if (s->pending)
			rc = -EINPROGRESS;
		else if (s->last_rc)
			rc = s->last_rc;
		else
			rc = s->known ? 0 : -ENOENT;
	}
	raw_spin_unlock_irqrestore(&netdev_coal_lock, flags);
	return rc;
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(netdev_coal_kfunc_ids)
BTF_ID_FLAGS(func, bpf_netdev_request_rx_coalesce)
BTF_ID_FLAGS(func, bpf_netdev_set_rx_coalesce, KF_SLEEPABLE)
BTF_ID_FLAGS(func, bpf_netdev_get_rx_coalesce)
BTF_KFUNCS_END(netdev_coal_kfunc_ids)

static const struct btf_kfunc_id_set netdev_coal_kfunc_set = {
	.owner	= THIS_MODULE,
	.set	= &netdev_coal_kfunc_ids,
};

static int __init netdev_coal_bpf_init(void)
{
	int ret;

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					&netdev_coal_kfunc_set);
	ret = ret ?: register_btf_kfunc_id_set(BPF_PROG_TYPE_SYSCALL,
					       &netdev_coal_kfunc_set);
	ret = ret ?: register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING,
					       &netdev_coal_kfunc_set);
	ret = ret ?: register_btf_kfunc_id_set(BPF_PROG_TYPE_SCHED_CLS,
					       &netdev_coal_kfunc_set);
	if (ret)
		pr_warn("netdev_coal: kfunc registration failed: %d\n", ret);
	return ret;
}
late_initcall(netdev_coal_bpf_init);
