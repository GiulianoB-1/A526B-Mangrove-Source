// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/base/dd.c - The core device/driver interactions.
 *
 * This file contains the (sometimes tricky) code that controls the
 * interactions between devices and drivers, which primarily includes
 * driver binding and unbinding.
 *
 * All of this code used to exist in drivers/base/bus.c, but was
 * relocated to here in the name of compartmentalization (since it wasn't
 * strictly code just for the 'struct bus_type'.
 *
 * Copyright (c) 2002-5 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007-2009 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007-2009 Novell Inc.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/string.h>
extern void a52_persistent_diag_mark(const char *fmt, ...);
extern void a52_ackfr_record(const char *fmt, ...);
extern void a52_device_links_force_probe(struct device *dev,
					 unsigned int *kept,
					 unsigned int *dropped);
extern void a52_device_links_trace(struct device *dev, const char *tag);
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/async.h>
#include <linux/pm_runtime.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/a52_ack_secure_flight_recorder.h>

#include "base.h"
#include "power/power.h"

/*
 * Deferred Probe infrastructure.
 *
 * Sometimes driver probe order matters, but the kernel doesn't always have
 * dependency information which means some drivers will get probed before a
 * resource it depends on is available.  For example, an SDHCI driver may
 * first need a GPIO line from an i2c GPIO controller before it can be
 * initialized.  If a required resource is not available yet, a driver can
 * request probing to be deferred by returning -EPROBE_DEFER from its probe hook
 *
 * Deferred probe maintains two lists of devices, a pending list and an active
 * list.  A driver returning -EPROBE_DEFER causes the device to be added to the
 * pending list.  A successful driver probe will trigger moving all devices
 * from the pending to the active list so that the workqueue will eventually
 * retry them.
 *
 * The deferred_probe_mutex must be held any time the deferred_probe_*_list
 * of the (struct device*)->p->deferred_probe pointers are manipulated
 */
static DEFINE_MUTEX(deferred_probe_mutex);
static LIST_HEAD(deferred_probe_pending_list);
static LIST_HEAD(deferred_probe_active_list);
static atomic_t deferred_trigger_count = ATOMIC_INIT(0);
static struct dentry *deferred_devices;
static bool initcalls_done;

/* Save the async probe drivers' name from kernel cmdline */
#define ASYNC_DRV_NAMES_MAX_LEN	256
static char async_probe_drv_names[ASYNC_DRV_NAMES_MAX_LEN];

/*
 * In some cases, like suspend to RAM or hibernation, It might be reasonable
 * to prohibit probing of devices as it could be unsafe.
 * Once defer_all_probes is true all drivers probes will be forcibly deferred.
 */
static bool defer_all_probes;

static bool a52_smmu_unsec_trace_dev(const struct device *dev)
{
	return dev && dev->of_node &&
		of_device_is_compatible(dev->of_node, "qcom,smmu_sde_unsec");
}

/* A52_PHASE230_KGSL_DRIVER_CORE_PATH
 * Observe only the exact qcom,kgsl-3d0 / kgsl-3d attach and probe path.
 * Bounded metadata records do not change matching, deferred-probe lists,
 * supplier decisions, callback order, return values, or device state.
 */
static atomic_t a52_r230_dd_records = ATOMIC_INIT(0);

static bool a52_r230_gpu_dev(const struct device *dev)
{
	return dev && dev->of_node &&
		of_device_is_compatible(dev->of_node, "qcom,kgsl-3d0");
}

static bool a52_r230_gpu_drv(const struct device_driver *drv)
{
	return drv && drv->name && !strcmp(drv->name, "kgsl-3d");
}

/* A52_PHASE238_CX_JOURNAL_DD_V1 */
static bool a52_r238_cx_dd_pair(const struct device *dev,
		const struct device_driver *drv)
{
	return dev && dev->of_node && dev_name(dev) && drv && drv->name &&
		strstr(dev_name(dev), "3d9106c") &&
		!strcmp(drv->name, "a52-legacy-gdsc-regulator");
}

/* A52_PHASE238_CX_DRIVER_WALK_V1 */
static atomic_t a52_r238_cx_walk_records = ATOMIC_INIT(0);

/* A52_PHASE240_CX_SUPPLIER_GATE_V1 */
static void a52_r240_cx_supplier_snapshot(struct device *dev,
		struct device_driver *drv)
{
	struct device_link *link;
	unsigned int n = 0;

	if (!a52_r238_cx_dd_pair(dev, drv))
		return;
	a52_ackfr_record("CXF240 sup-in d=%.20s r=%.20s ls=%d",
			dev_name(dev), drv->name, dev->links.status);
	list_for_each_entry(link, &dev->links.suppliers, c_node) {
		const char *sname;
		const char *sdrv;

		if (++n > 24) {
			a52_ackfr_record("CXF240 sup-limit n=%u", n);
			break;
		}
		sname = link->supplier ? dev_name(link->supplier) : "-";
		sdrv = link->supplier && link->supplier->driver &&
			link->supplier->driver->name ?
			link->supplier->driver->name : "-";
		a52_ackfr_record(
			"CXF240 sup n=%u s=%.20s r=%.20s st=%u fl=%x ds=%d",
			n, sname, sdrv, link->status, link->flags,
			link->supplier ? link->supplier->links.status : -1);
	}
}

static bool a52_r238_cx_walk_dev(const struct device *dev)
{
	return dev && dev->of_node && dev_name(dev) &&
		strstr(dev_name(dev), "3d9106c");
}

static bool a52_r238_cx_walk_drv(const struct device_driver *drv)
{
	return drv && drv->name &&
		!strcmp(drv->name, "a52-legacy-gdsc-regulator");
}

#define A52_R238_CXW(_fmt, ...) \
	do { \
		if (atomic_inc_return(&a52_r238_cx_walk_records) <= 24) \
			A52_R230_DD("cxw " _fmt, ##__VA_ARGS__); \
	} while (0)

static bool a52_r230_gpu_pair(const struct device *dev,
		const struct device_driver *drv)
{
	return (a52_r230_gpu_dev(dev) && a52_r230_gpu_drv(drv)) ||
		a52_r238_cx_dd_pair(dev, drv);
}

static bool a52_r230_dd_take(void)
{
	return atomic_inc_return(&a52_r230_dd_records) <= 160;
}

#define A52_R230_DD(_fmt, ...) \
	do { \
		if (a52_r230_dd_take()) \
			a52_ackfr_record("KGPPOST 230 " _fmt, ##__VA_ARGS__); \
	} while (0)

static const char *a52_r230_reason(const struct device *dev)
{
	return dev && dev->p && dev->p->deferred_probe_reason ?
		dev->p->deferred_probe_reason : "-";
}


/*
 * deferred_probe_work_func() - Retry probing devices in the active list.
 */
static void deferred_probe_work_func(struct work_struct *work)
{
	struct device *dev;
	struct device_private *private;
	/*
	 * This block processes every device in the deferred 'active' list.
	 * Each device is removed from the active list and passed to
	 * bus_probe_device() to re-attempt the probe.  The loop continues
	 * until every device in the active list is removed and retried.
	 *
	 * Note: Once the device is removed from the list and the mutex is
	 * released, it is possible for the device get freed by another thread
	 * and cause a illegal pointer dereference.  This code uses
	 * get/put_device() to ensure the device structure cannot disappear
	 * from under our feet.
	 */
	mutex_lock(&deferred_probe_mutex);
	while (!list_empty(&deferred_probe_active_list)) {
		private = list_first_entry(&deferred_probe_active_list,
					typeof(*dev->p), deferred_probe);
		dev = private->device;
		if (a52_r230_gpu_dev(dev))
			A52_R230_DD("def-retry d=%.24s reason=%.28s",
				dev_name(dev), a52_r230_reason(dev));
		list_del_init(&private->deferred_probe);

		get_device(dev);

		kfree(dev->p->deferred_probe_reason);
		dev->p->deferred_probe_reason = NULL;

		/*
		 * Drop the mutex while probing each device; the probe path may
		 * manipulate the deferred list
		 */
		mutex_unlock(&deferred_probe_mutex);

		/*
		 * Force the device to the end of the dpm_list since
		 * the PM code assumes that the order we add things to
		 * the list is a good order for suspend but deferred
		 * probe makes that very unsafe.
		 */
		device_pm_move_to_tail(dev);

		dev_dbg(dev, "Retrying from deferred list\n");
		if (a52_smmu_unsec_trace_dev(dev))
			a52_ackfr_record("DCORE defer retry dev=%s", dev_name(dev));
		bus_probe_device(dev);
		mutex_lock(&deferred_probe_mutex);

		put_device(dev);
	}
	mutex_unlock(&deferred_probe_mutex);
}
static DECLARE_WORK(deferred_probe_work, deferred_probe_work_func);

static bool a52_storage_probe_device(const struct device *dev)
{
	const char *name;

	if (!dev)
		return false;
	name = dev_name(dev);
	return name && (strstr(name, "ufs") || strstr(name, "scsi") ||
			strstr(name, "sdhci") || strstr(name, "1d84000") ||
			strstr(name, "1d87000") || strstr(name, "f100000"));
}

static bool a52_display_probe_device(const struct device *dev)
{
	if (!dev || !dev->of_node)
		return false;

	return of_device_is_compatible(dev->of_node, "qcom,sde-kms") ||
	       of_device_is_compatible(dev->of_node, "qcom,dsi-display") ||
	       of_device_is_compatible(dev->of_node, "qcom,dsi-ctrl-hw-v2.4");
}

static bool a52_rscc_probe_device(const struct device *dev)
{
	return dev && dev->of_node &&
		of_device_is_compatible(dev->of_node, "qcom,sde-rsc");
}

/* A52_PHASE234_RSCC_MATCH_FILTER_V1 */
static bool a52_rscc_probe_driver(const struct device_driver *drv)
{
	return drv && drv->name && !strcmp(drv->name, "sde_rsc");
}

void driver_deferred_probe_add(struct device *dev)
{
	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE defer add dev=%s", dev_name(dev));
	if (a52_rscc_probe_device(dev))
		a52_ackfr_record("RSCCCORE deferred-add dev=%s reason=%s",
			dev_name(dev), dev->p && dev->p->deferred_probe_reason ?
			dev->p->deferred_probe_reason : "-");
	if (a52_storage_probe_device(dev)) {
		do { } while (0);
		do { } while (0);
		do { } while (0);
	}
	mutex_lock(&deferred_probe_mutex);
	if (a52_r230_gpu_dev(dev))
		A52_R230_DD("def-add d=%.24s empty=%d reason=%.28s",
			dev_name(dev), list_empty(&dev->p->deferred_probe),
			a52_r230_reason(dev));
	if (list_empty(&dev->p->deferred_probe)) {
		dev_dbg(dev, "Added to deferred list\n");
		list_add_tail(&dev->p->deferred_probe, &deferred_probe_pending_list);
	}
	mutex_unlock(&deferred_probe_mutex);
}

void driver_deferred_probe_del(struct device *dev)
{
	mutex_lock(&deferred_probe_mutex);
	if (a52_r230_gpu_dev(dev))
		A52_R230_DD("def-del d=%.24s listed=%d reason=%.28s",
			dev_name(dev), !list_empty(&dev->p->deferred_probe),
			a52_r230_reason(dev));
	if (!list_empty(&dev->p->deferred_probe)) {
		dev_dbg(dev, "Removed from deferred list\n");
		list_del_init(&dev->p->deferred_probe);
		kfree(dev->p->deferred_probe_reason);
		dev->p->deferred_probe_reason = NULL;
	}
	mutex_unlock(&deferred_probe_mutex);
}

static bool driver_deferred_probe_enable = false;
/**
 * driver_deferred_probe_trigger() - Kick off re-probing deferred devices
 *
 * This functions moves all devices from the pending list to the active
 * list and schedules the deferred probe workqueue to process them.  It
 * should be called anytime a driver is successfully bound to a device.
 *
 * Note, there is a race condition in multi-threaded probe. In the case where
 * more than one device is probing at the same time, it is possible for one
 * probe to complete successfully while another is about to defer. If the second
 * depends on the first, then it will get put on the pending list after the
 * trigger event has already occurred and will be stuck there.
 *
 * The atomic 'deferred_trigger_count' is used to determine if a successful
 * trigger has occurred in the midst of probing a driver. If the trigger count
 * changes in the midst of a probe, then deferred processing should be triggered
 * again.
 */
static void driver_deferred_probe_trigger(void)
{
	if (!driver_deferred_probe_enable)
		return;

	/*
	 * A successful probe means that all the devices in the pending list
	 * should be triggered to be reprobed.  Move all the deferred devices
	 * into the active list so they can be retried by the workqueue
	 */
	mutex_lock(&deferred_probe_mutex);
	atomic_inc(&deferred_trigger_count);
	list_splice_tail_init(&deferred_probe_pending_list,
			      &deferred_probe_active_list);
	mutex_unlock(&deferred_probe_mutex);

	/*
	 * Kick the re-probe thread.  It may already be scheduled, but it is
	 * safe to kick it again.
	 */
	queue_work(system_unbound_wq, &deferred_probe_work);
}

/**
 * device_block_probing() - Block/defer device's probes
 *
 *	It will disable probing of devices and defer their probes instead.
 */
void device_block_probing(void)
{
	defer_all_probes = true;
	/* sync with probes to avoid races. */
	wait_for_device_probe();
}

/**
 * device_unblock_probing() - Unblock/enable device's probes
 *
 *	It will restore normal behavior and trigger re-probing of deferred
 * devices.
 */
void device_unblock_probing(void)
{
	defer_all_probes = false;
	driver_deferred_probe_trigger();
}

/**
 * device_set_deferred_probe_reason() - Set defer probe reason message for device
 * @dev: the pointer to the struct device
 * @vaf: the pointer to va_format structure with message
 */
void device_set_deferred_probe_reason(const struct device *dev, struct va_format *vaf)
{
	const char *drv = dev_driver_string(dev);

	mutex_lock(&deferred_probe_mutex);

	kfree(dev->p->deferred_probe_reason);
	dev->p->deferred_probe_reason = kasprintf(GFP_KERNEL, "%s: %pV", drv, vaf);

	mutex_unlock(&deferred_probe_mutex);
}

/*
 * deferred_devs_show() - Show the devices in the deferred probe pending list.
 */
static int deferred_devs_show(struct seq_file *s, void *data)
{
	struct device_private *curr;

	mutex_lock(&deferred_probe_mutex);

	list_for_each_entry(curr, &deferred_probe_pending_list, deferred_probe)
		seq_printf(s, "%s\t%s", dev_name(curr->device),
			   curr->device->p->deferred_probe_reason ?: "\n");

	mutex_unlock(&deferred_probe_mutex);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(deferred_devs);

int driver_deferred_probe_timeout;
EXPORT_SYMBOL_GPL(driver_deferred_probe_timeout);

static int __init deferred_probe_timeout_setup(char *str)
{
	int timeout;

	if (!kstrtoint(str, 10, &timeout))
		driver_deferred_probe_timeout = timeout;
	return 1;
}
__setup("deferred_probe_timeout=", deferred_probe_timeout_setup);

/**
 * driver_deferred_probe_check_state() - Check deferred probe state
 * @dev: device to check
 *
 * Return:
 * -ENODEV if initcalls have completed and modules are disabled.
 * -ETIMEDOUT if the deferred probe timeout was set and has expired
 *  and modules are enabled.
 * -EPROBE_DEFER in other cases.
 *
 * Drivers or subsystems can opt-in to calling this function instead of directly
 * returning -EPROBE_DEFER.
 */
int driver_deferred_probe_check_state(struct device *dev)
{
	if (!IS_ENABLED(CONFIG_MODULES) && initcalls_done) {
		dev_warn(dev, "ignoring dependency for device, assuming no driver\n");
		return -ENODEV;
	}

	if (!driver_deferred_probe_timeout && initcalls_done) {
		dev_warn(dev, "deferred probe timeout, ignoring dependency\n");
		return -ETIMEDOUT;
	}

	return -EPROBE_DEFER;
}

static void deferred_probe_timeout_work_func(struct work_struct *work)
{
	struct device_private *p;

	driver_deferred_probe_timeout = 0;
	driver_deferred_probe_trigger();
	flush_work(&deferred_probe_work);

	mutex_lock(&deferred_probe_mutex);
	list_for_each_entry(p, &deferred_probe_pending_list, deferred_probe)
		dev_info(p->device, "deferred probe pending\n");
	mutex_unlock(&deferred_probe_mutex);
}
static DECLARE_DELAYED_WORK(deferred_probe_timeout_work, deferred_probe_timeout_work_func);

/**
 * deferred_probe_initcall() - Enable probing of deferred devices
 *
 * We don't want to get in the way when the bulk of drivers are getting probed.
 * Instead, this initcall makes sure that deferred probing is delayed until
 * late_initcall time.
 */
static int deferred_probe_initcall(void)
{
	deferred_devices = debugfs_create_file("devices_deferred", 0444, NULL,
					       NULL, &deferred_devs_fops);

	driver_deferred_probe_enable = true;
	driver_deferred_probe_trigger();
	/* Sort as many dependencies as possible before exiting initcalls */
	flush_work(&deferred_probe_work);
	initcalls_done = true;

	/*
	 * Trigger deferred probe again, this time we won't defer anything
	 * that is optional
	 */
	driver_deferred_probe_trigger();
	flush_work(&deferred_probe_work);

	if (driver_deferred_probe_timeout > 0) {
		schedule_delayed_work(&deferred_probe_timeout_work,
			driver_deferred_probe_timeout * HZ);
	}
	return 0;
}
late_initcall(deferred_probe_initcall);

static void __exit deferred_probe_exit(void)
{
	debugfs_remove_recursive(deferred_devices);
}
__exitcall(deferred_probe_exit);

/**
 * device_is_bound() - Check if device is bound to a driver
 * @dev: device to check
 *
 * Returns true if passed device has already finished probing successfully
 * against a driver.
 *
 * This function must be called with the device lock held.
 */
bool device_is_bound(struct device *dev)
{
	return dev->p && klist_node_attached(&dev->p->knode_driver);
}

static void driver_bound(struct device *dev)
{
	if (device_is_bound(dev)) {
		pr_warn("%s: device %s already bound\n",
			__func__, kobject_name(&dev->kobj));
		return;
	}

	pr_debug("driver: '%s': %s: bound to device '%s'\n", dev->driver->name,
		 __func__, dev_name(dev));

	klist_add_tail(&dev->p->knode_driver, &dev->driver->p->klist_devices);
	device_links_driver_bound(dev);

	device_pm_check_callbacks(dev);

	/*
	 * Make sure the device is no longer in one of the deferred lists and
	 * kick off retrying all pending devices
	 */
	driver_deferred_probe_del(dev);
	driver_deferred_probe_trigger();

	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_BOUND_DRIVER, dev);

	kobject_uevent(&dev->kobj, KOBJ_BIND);
}

static ssize_t coredump_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	device_lock(dev);
	dev->driver->coredump(dev);
	device_unlock(dev);

	return count;
}
static DEVICE_ATTR_WO(coredump);

static int driver_sysfs_add(struct device *dev)
{
	int ret;

	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_BIND_DRIVER, dev);

	ret = sysfs_create_link(&dev->driver->p->kobj, &dev->kobj,
				kobject_name(&dev->kobj));
	if (ret)
		goto fail;

	ret = sysfs_create_link(&dev->kobj, &dev->driver->p->kobj,
				"driver");
	if (ret)
		goto rm_dev;

	if (!IS_ENABLED(CONFIG_DEV_COREDUMP) || !dev->driver->coredump ||
	    !device_create_file(dev, &dev_attr_coredump))
		return 0;

	sysfs_remove_link(&dev->kobj, "driver");

rm_dev:
	sysfs_remove_link(&dev->driver->p->kobj,
			  kobject_name(&dev->kobj));

fail:
	return ret;
}

static void driver_sysfs_remove(struct device *dev)
{
	struct device_driver *drv = dev->driver;

	if (drv) {
		if (drv->coredump)
			device_remove_file(dev, &dev_attr_coredump);
		sysfs_remove_link(&drv->p->kobj, kobject_name(&dev->kobj));
		sysfs_remove_link(&dev->kobj, "driver");
	}
}

/**
 * device_bind_driver - bind a driver to one device.
 * @dev: device.
 *
 * Allow manual attachment of a driver to a device.
 * Caller must have already set @dev->driver.
 *
 * Note that this does not modify the bus reference count.
 * Please verify that is accounted for before calling this.
 * (It is ok to call with no other effort from a driver's probe() method.)
 *
 * This function must be called with the device lock held.
 */
int device_bind_driver(struct device *dev)
{
	int ret;

	ret = driver_sysfs_add(dev);
	if (!ret)
		driver_bound(dev);
	else if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_DRIVER_NOT_BOUND, dev);
	return ret;
}
EXPORT_SYMBOL_GPL(device_bind_driver);

static atomic_t probe_count = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(probe_waitqueue);

static void driver_deferred_probe_add_trigger(struct device *dev,
					      int local_trigger_count)
{
	driver_deferred_probe_add(dev);
	/* Did a trigger occur while probing? Need to re-trigger if yes */
	if (local_trigger_count != atomic_read(&deferred_trigger_count))
		driver_deferred_probe_trigger();
}

static ssize_t state_synced_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	bool val;

	device_lock(dev);
	val = dev->state_synced;
	device_unlock(dev);

	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(state_synced);

static bool a52_legacy_fw_devlink_consumer(const struct device *dev)
{
	const char *name;

	if (!dev)
		return false;
	name = dev_name(dev);
	return name && !strcmp(name, "1d84000.ufshc");
}

static bool a52_legacy_ufs_named_reset_pinctrl(const struct device *dev)
{
	if (!dev || !dev->of_node || strcmp(dev_name(dev), "1d84000.ufshc"))
		return false;

	return of_property_match_string(dev->of_node, "pinctrl-names",
					"dev-reset-assert") >= 0 &&
	       of_property_match_string(dev->of_node, "pinctrl-names",
					"dev-reset-deassert") >= 0;
}

static bool a52_run40_preprobe_target(const struct device *dev)
{
	const char *name = dev ? dev_name(dev) : NULL;

	return name && !strcmp(name, "1d84000.ufshc");
}


/* A52_PHASE238_DRIVER_CORE_GPU_TRACE_V1 */
#define A52_G238_DD_LIMIT 768
static atomic_t a52_g238_dd_events = ATOMIC_INIT(0);

static bool a52_g238_dd_focus(struct device *dev)
{
	const char *n;

	if (!dev)
		return false;
	n = dev_name(dev);
	return n && (strstr(n, "3d00000") || strstr(n, "3d90000") ||
		     strstr(n, "3d9100c") || strstr(n, "3d9106c") ||
		     strstr(n, "780000") || strstr(n, "c350000") ||
		     strstr(n, "kgsl") || strstr(n, "gpu") ||
		     strstr(n, "gdsc") || strstr(n, "qfprom"));
}

static bool a52_g238_dd_can_log(struct device *dev)
{
	int n;

	if (!a52_g238_dd_focus(dev))
		return false;
	n = atomic_inc_return(&a52_g238_dd_events);
	return n <= A52_G238_DD_LIMIT ||
	       (dev_name(dev) && strstr(dev_name(dev), "3d9106c"));
}

static void a52_g238_dd_dump_suppliers(struct device *dev, struct device_driver *drv)
{
	struct device_link *link;
	int n = 0;

	if (!a52_g238_dd_can_log(dev))
		return;
	a52_ackfr_record("G238 D in dev=%s drv=%s cur=%s",
			 dev_name(dev), drv ? drv->name : "-",
			 dev->driver ? dev->driver->name : "-");
	list_for_each_entry(link, &dev->links.suppliers, c_node) {
		if (++n > 32) {
			a52_ackfr_record("G238 D slimit dev=%s n=%d", dev_name(dev), n);
			break;
		}
		a52_ackfr_record("G238 D sup c=%s s=%s drv=%s st=%u fl=%u",
				 dev_name(dev), dev_name(link->supplier),
				 link->supplier->driver ? link->supplier->driver->name : "-",
				 link->status, link->flags);
	}
}

static void a52_g238_dd_supplier_result(struct device *dev, int rc)
{
	if (a52_g238_dd_focus(dev))
		a52_ackfr_record("G238 D sup-out dev=%s rc=%d", dev_name(dev), rc);
}

static int a52_g238_dd_return(struct device *dev, int rc, int line)
{
	if (a52_g238_dd_focus(dev))
		a52_ackfr_record("G238 D out dev=%s rc=%d l=%d cur=%s",
				 dev_name(dev), rc, line,
				 dev->driver ? dev->driver->name : "-");
	return rc;
}

/* A52_PHASE243_CXGX_LIVE_SUPPLIER_V1 */
static char a52_r243_dd_tag(const struct device *dev,
		const struct device_driver *drv)
{
	const char *name;

	if (!dev || !drv || !drv->name ||
	    strcmp(drv->name, "a52-legacy-gdsc-regulator"))
		return 0;
	name = dev_name(dev);
	if (!name)
		return 0;
	if (strstr(name, "3d9106c"))
		return 'C';
	if (strstr(name, "3d9100c"))
		return 'G';
	return 0;
}

static void a52_r243_rp_enter3(struct device *dev,
		struct device_driver *drv)
{
	char tag = a52_r243_dd_tag(dev, drv);
	int q;

	if (!tag)
		return;
	for (q = 0; q < 3; q++)
		a52_ackfr_record("CXF243 R c=%c q=%d ls=%d", tag, q,
			dev->links.status);
}

static void a52_r243_rp_links3(struct device *dev,
		struct device_driver *drv)
{
	struct device_link *link;
	char tag = a52_r243_dd_tag(dev, drv);
	int n = 0;
	int q;

	if (!tag)
		return;
	list_for_each_entry(link, &dev->links.suppliers, c_node) {
		const char *s = link->supplier ? dev_name(link->supplier) : "-";
		int ds = link->supplier ? (int)link->supplier->links.status : -1; /* A52_PHASE243_DL_STATE_CAST_V1 */
		int bound = link->supplier && link->supplier->driver;

		if (++n > 8)
			break;
		for (q = 0; q < 3; q++)
			a52_ackfr_record(
				"CXF243 L c=%c q=%d n=%d s=%.36s st=%u ds=%d b=%d",
				tag, q, n, s, link->status, ds, bound);
	}
}

static void a52_r243_rp_gate3(struct device *dev,
		struct device_driver *drv, int rc)
{
	char tag = a52_r243_dd_tag(dev, drv);
	int q;

	if (!tag)
		return;
	for (q = 0; q < 3; q++)
		a52_ackfr_record("CXF243 G c=%c q=%d rc=%d ls=%d", tag, q,
			rc, dev->links.status);
}

static int really_probe(struct device *dev, struct device_driver *drv)
{
	int ret = -EPROBE_DEFER;
	int local_trigger_count = atomic_read(&deferred_trigger_count);
	bool test_remove = IS_ENABLED(CONFIG_DEBUG_TEST_DRIVER_REMOVE) &&
			   !drv->suppress_bind_attrs;

	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("rp-in d=%.24s r=%.16s all=%d dead=%d cur=%.16s",
			dev_name(dev), drv->name, defer_all_probes,
			dev->p ? dev->p->dead : -1,
			dev->driver && dev->driver->name ? dev->driver->name : "-");

	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE really enter dev=%s drv=%s all=%d",
			dev_name(dev), drv->name, defer_all_probes);

	if (a52_rscc_probe_device(dev)) {
		a52_ackfr_record("RSCCCORE really-probe enter dev=%s drv=%s",
			dev_name(dev), drv && drv->name ? drv->name : "-");
		a52_device_links_trace(dev, "really-enter");
	}

	if (a52_display_probe_device(dev))
		a52_ackfr_record("DISP RP enter dev=%s drv=%s",
			dev_name(dev), drv && drv->name ? drv->name : "-");

	if (defer_all_probes) {
		/*
		 * Value of defer_all_probes can be set only by
		 * device_block_probing() which, in turn, will call
		 * wait_for_device_probe() right after that to avoid any races.
		 */
		dev_dbg(dev, "Driver %s force probe deferral\n", drv->name);
		driver_deferred_probe_add(dev);
		return a52_g238_dd_return(dev, (ret), __LINE__);
	}

	a52_g238_dd_dump_suppliers(dev, drv);
	a52_r240_cx_supplier_snapshot(dev, drv);
	a52_r243_rp_enter3(dev, drv);
	a52_r243_rp_links3(dev, drv);
	ret = device_links_check_suppliers(dev);
	a52_r243_rp_gate3(dev, drv, ret);
	if (a52_r238_cx_dd_pair(dev, drv))
		a52_ackfr_record("CXF240 sup-out d=%.20s rc=%d ls=%d",
			dev_name(dev), ret, dev->links.status);
	a52_g238_dd_supplier_result(dev, ret);
	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("rp-sup rc=%d ls=%d reason=%.32s", ret,
			dev->links.status, a52_r230_reason(dev));
	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE suppliers rc=%d status=%d",
			ret, dev->links.status);
	if (a52_rscc_probe_device(dev)) {
		const char *reason = dev->p && dev->p->deferred_probe_reason ?
			dev->p->deferred_probe_reason : "-";

		a52_ackfr_record("RSCCCORE suppliers dev=%s rc=%d reason=%s",
			dev_name(dev), ret, reason);
		a52_device_links_trace(dev, "suppliers-checked");
	}
	if (ret == -EPROBE_DEFER && a52_legacy_fw_devlink_consumer(dev)) {
		const char *reason = dev->p && dev->p->deferred_probe_reason ?
			dev->p->deferred_probe_reason : "<none>";
		unsigned int kept = 0;
		unsigned int dropped = 0;

		/*
		 * Run 35 proved that the real QMP PHY provider is bound, while the
		 * legacy UFS host is still rejected before ufs_qcom_probe(). Purge
		 * only this consumer's unresolved firmware links, drop unavailable
		 * managed device links, and transition available links to
		 * CONSUMER_PROBE so device_links_driver_bound/no_driver() remain
		 * internally consistent.
		 */
		a52_device_links_force_probe(dev, &kept, &dropped);
		a52_persistent_diag_mark("A52_UFS_FW_DEVLINK_FORCE_PROBE copy=1 dev=%s driver=%s reason=%s kept=%u dropped=%u\n",
			dev_name(dev), drv->name, reason, kept, dropped);
		a52_persistent_diag_mark("A52_UFS_FW_DEVLINK_FORCE_PROBE copy=2 dev=%s driver=%s reason=%s kept=%u dropped=%u\n",
			dev_name(dev), drv->name, reason, kept, dropped);
		a52_persistent_diag_mark("A52_UFS_FW_DEVLINK_FORCE_PROBE copy=3 dev=%s driver=%s reason=%s kept=%u dropped=%u\n",
			dev_name(dev), drv->name, reason, kept, dropped);
		ret = 0;
	}
	if (a52_display_probe_device(dev)) {
		const char *reason = dev->p && dev->p->deferred_probe_reason ?
			dev->p->deferred_probe_reason : "-";

		a52_ackfr_record("DISP RP suppliers dev=%s rc=%d reason=%s",
			dev_name(dev), ret, reason);
	}
	if (ret == -EPROBE_DEFER && dev->of_node &&
	    of_device_is_compatible(dev->of_node, "qcom,dsi-ctrl-hw-v2.4"))
		a52_ackfr_record("DISP RP defer-preserved dev=%s rc=%d",
			dev_name(dev), ret);
	if (ret == -EPROBE_DEFER && a52_display_probe_device(dev))
		a52_ackfr_record("DISP RP defer-normal dev=%s rc=%d",
			dev_name(dev), ret);
	if (ret == -EPROBE_DEFER)
		driver_deferred_probe_add_trigger(dev, local_trigger_count);
	if (ret) {
		if (a52_r230_gpu_pair(dev, drv))
			A52_R230_DD("rp-stop stage=sup rc=%d reason=%.32s",
				ret, a52_r230_reason(dev));
		return a52_g238_dd_return(dev, (ret), __LINE__);
	}

	atomic_inc(&probe_count);
	if (a52_storage_probe_device(dev)) {
		do { } while (0);
		do { } while (0);
		do { } while (0);
	}
	pr_debug("bus: '%s': %s: probing driver %s with device %s\n",
		 drv->bus->name, __func__, drv->name, dev_name(dev));
	if (!list_empty(&dev->devres_head)) {
		dev_crit(dev, "Resources present before probing\n");
		ret = -EBUSY;
		goto done;
	}

re_probe:
	dev->driver = drv;

	/* If using pinctrl, bind pins now before probing */
	ret = pinctrl_bind_pins(dev);
	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("rp-pin rc=%d", ret);
	if (a52_rscc_probe_device(dev))
		a52_ackfr_record("RSCCCORE pinctrl dev=%s rc=%d", dev_name(dev), ret);
	if (a52_display_probe_device(dev))
		a52_ackfr_record("DISP RP pinctrl dev=%s rc=%d", dev_name(dev), ret);
	if (a52_run40_preprobe_target(dev)) {
		a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=pinctrl dev=%s driver=%s ret=%d pins=%s\n", dev_name(dev), drv->name, ret, dev->pins ? "present" : "none");
		a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=pinctrl dev=%s driver=%s ret=%d pins=%s\n", dev_name(dev), drv->name, ret, dev->pins ? "present" : "none");
		a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=pinctrl dev=%s driver=%s ret=%d pins=%s\n", dev_name(dev), drv->name, ret, dev->pins ? "present" : "none");
	}
	if (ret == -EPROBE_DEFER && a52_legacy_ufs_named_reset_pinctrl(dev)) {
/* A52_PHASE361_UFS_RETAINED_TRACE_V1 */
		a52_persistent_diag_mark("A52_UFS_PINCTRL_DEFER_BYPASS copy=1 dev=%s driver=%s ret=%d contract=named-reset\n", dev_name(dev), drv->name, ret);
		a52_persistent_diag_mark("A52_UFS_PINCTRL_DEFER_BYPASS copy=2 dev=%s driver=%s ret=%d contract=named-reset\n", dev_name(dev), drv->name, ret);
		a52_persistent_diag_mark("A52_UFS_PINCTRL_DEFER_BYPASS copy=3 dev=%s driver=%s ret=%d contract=named-reset\n", dev_name(dev), drv->name, ret);
		/*
		 * The Samsung DT has no generic default/init state. The UFS variant
		 * driver owns the two explicit reset states and selects them during
		 * device reset. pinctrl_bind_pins() has already cleaned dev->pins on
		 * this deferral, so continue without retaining a partial handle.
		 */
		ret = 0;
	}
	if (ret)
		goto pinctrl_bind_failed;

	if (dev->bus->dma_configure) {
		if (a52_smmu_unsec_trace_dev(dev))
			a52_ackfr_record("DCORE dma enter dev=%s", dev_name(dev));
		ret = dev->bus->dma_configure(dev);
		if (a52_r230_gpu_pair(dev, drv))
			A52_R230_DD("rp-dma rc=%d mapped=%d", ret,
				device_iommu_mapped(dev));
		if (a52_smmu_unsec_trace_dev(dev))
			a52_ackfr_record("DCORE dma exit rc=%d mapped=%d", ret,
				device_iommu_mapped(dev));
		if (a52_rscc_probe_device(dev))
			a52_ackfr_record("RSCCCORE dma dev=%s rc=%d", dev_name(dev), ret);
		if (a52_display_probe_device(dev))
			a52_ackfr_record("DISP RP dma dev=%s rc=%d", dev_name(dev), ret);
		if (a52_run40_preprobe_target(dev)) {
			a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=dma dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
			a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=dma dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
			a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=dma dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
		}
		if (ret)
			goto probe_failed;
	} else if (a52_run40_preprobe_target(dev)) {
		a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=dma dev=%s driver=%s ret=0 hook=none\n", dev_name(dev), drv->name);
		a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=dma dev=%s driver=%s ret=0 hook=none\n", dev_name(dev), drv->name);
		a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=dma dev=%s driver=%s ret=0 hook=none\n", dev_name(dev), drv->name);
	}

	ret = driver_sysfs_add(dev);
	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("rp-sys rc=%d", ret);
	if (a52_rscc_probe_device(dev))
		a52_ackfr_record("RSCCCORE sysfs dev=%s rc=%d", dev_name(dev), ret);
	if (a52_display_probe_device(dev))
		a52_ackfr_record("DISP RP sysfs dev=%s rc=%d", dev_name(dev), ret);
	if (a52_run40_preprobe_target(dev)) {
		a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=sysfs dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
		a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=sysfs dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
		a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=sysfs dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
	}
	if (ret) {
		pr_err("%s: driver_sysfs_add(%s) failed\n",
		       __func__, dev_name(dev));
		goto probe_failed;
	}

	if (dev->pm_domain && dev->pm_domain->activate) {
		ret = dev->pm_domain->activate(dev);
		if (a52_r230_gpu_pair(dev, drv))
			A52_R230_DD("rp-pm rc=%d", ret);
		if (a52_rscc_probe_device(dev))
			a52_ackfr_record("RSCCCORE pm dev=%s rc=%d", dev_name(dev), ret);
		if (a52_display_probe_device(dev))
			a52_ackfr_record("DISP RP pm dev=%s rc=%d", dev_name(dev), ret);
		if (a52_run40_preprobe_target(dev)) {
			a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=pm_activate dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
			a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=pm_activate dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
			a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=pm_activate dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
		}
		if (ret)
			goto probe_failed;
	} else if (a52_run40_preprobe_target(dev)) {
		a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=pm_activate dev=%s driver=%s ret=0 hook=none\n", dev_name(dev), drv->name);
		a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=pm_activate dev=%s driver=%s ret=0 hook=none\n", dev_name(dev), drv->name);
		a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=pm_activate dev=%s driver=%s ret=0 hook=none\n", dev_name(dev), drv->name);
	}

	if (dev->bus->probe) {
		if (a52_rscc_probe_device(dev))
			a52_ackfr_record("RSCCCORE busprobe enter dev=%s drv=%s",
				dev_name(dev), drv && drv->name ? drv->name : "-");
		if (a52_run40_preprobe_target(dev)) {
			a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=bus_probe_begin dev=%s driver=%s\n", dev_name(dev), drv->name);
			a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=bus_probe_begin dev=%s driver=%s\n", dev_name(dev), drv->name);
			a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=bus_probe_begin dev=%s driver=%s\n", dev_name(dev), drv->name);
		}
		if (a52_display_probe_device(dev))
			a52_ackfr_record("DISP RP busprobe enter dev=%s drv=%s",
				dev_name(dev), drv && drv->name ? drv->name : "-");
		if (a52_smmu_unsec_trace_dev(dev))
			a52_ackfr_record("DCORE bus-probe enter drv=%s", drv->name);
		if (a52_r230_gpu_pair(dev, drv))
			A52_R230_DD("rp-cb in bus=1");
		ret = dev->bus->probe(dev);
		if (a52_r230_gpu_pair(dev, drv))
			A52_R230_DD("rp-cb out rc=%d", ret);
		if (a52_smmu_unsec_trace_dev(dev))
			a52_ackfr_record("DCORE bus-probe exit rc=%d", ret);
		if (a52_rscc_probe_device(dev))
			a52_ackfr_record("RSCCCORE busprobe exit dev=%s rc=%d",
				dev_name(dev), ret);
		if (a52_display_probe_device(dev))
			a52_ackfr_record("DISP RP busprobe exit dev=%s rc=%d",
				dev_name(dev), ret);
		if (a52_run40_preprobe_target(dev)) {
			a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=bus_probe_end dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
			a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=bus_probe_end dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
			a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=bus_probe_end dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
		}
		if (ret)
			goto probe_failed;
	} else if (drv->probe) {
		if (a52_run40_preprobe_target(dev)) {
			a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=driver_probe_begin dev=%s driver=%s\n", dev_name(dev), drv->name);
			a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=driver_probe_begin dev=%s driver=%s\n", dev_name(dev), drv->name);
			a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=driver_probe_begin dev=%s driver=%s\n", dev_name(dev), drv->name);
		}
		if (a52_display_probe_device(dev))
			a52_ackfr_record("DISP RP drvprobe enter dev=%s drv=%s",
				dev_name(dev), drv && drv->name ? drv->name : "-");
		if (a52_r230_gpu_pair(dev, drv))
			A52_R230_DD("rp-cb in bus=0");
		ret = drv->probe(dev);
		if (a52_r230_gpu_pair(dev, drv))
			A52_R230_DD("rp-cb out rc=%d", ret);
		if (a52_display_probe_device(dev))
			a52_ackfr_record("DISP RP drvprobe exit dev=%s rc=%d",
				dev_name(dev), ret);
		if (a52_run40_preprobe_target(dev)) {
			a52_persistent_diag_mark("A52GDSC P361 c=1 PREPROBE stage=driver_probe_end dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
			a52_persistent_diag_mark("A52GDSC P361 c=2 PREPROBE stage=driver_probe_end dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
			a52_persistent_diag_mark("A52GDSC P361 c=3 PREPROBE stage=driver_probe_end dev=%s driver=%s ret=%d\n", dev_name(dev), drv->name, ret);
		}
		if (ret)
			goto probe_failed;
	}

	ret = device_add_groups(dev, drv->dev_groups);
	if (ret) {
		dev_err(dev, "device_add_groups() failed\n");
		goto dev_groups_failed;
	}

	if (dev_has_sync_state(dev)) {
		ret = device_create_file(dev, &dev_attr_state_synced);
		if (ret) {
			dev_err(dev, "state_synced sysfs add failed\n");
			goto dev_sysfs_state_synced_failed;
		}
	}

	if (test_remove) {
		test_remove = false;

		device_remove_file(dev, &dev_attr_state_synced);
		device_remove_groups(dev, drv->dev_groups);

		if (dev->bus->remove)
			dev->bus->remove(dev);
		else if (drv->remove)
			drv->remove(dev);

		devres_release_all(dev);
		arch_teardown_dma_ops(dev);
		kfree(dev->dma_range_map);
		dev->dma_range_map = NULL;
		driver_sysfs_remove(dev);
		dev->driver = NULL;
		dev_set_drvdata(dev, NULL);
		if (dev->pm_domain && dev->pm_domain->dismiss)
			dev->pm_domain->dismiss(dev);
		pm_runtime_reinit(dev);

		goto re_probe;
	}

	pinctrl_init_done(dev);

	if (dev->pm_domain && dev->pm_domain->sync)
		dev->pm_domain->sync(dev);

	driver_bound(dev);
	ret = 1;
	pr_debug("bus: '%s': %s: bound device %s to driver %s\n",
		 drv->bus->name, __func__, dev_name(dev), drv->name);
	goto done;

dev_sysfs_state_synced_failed:
	device_remove_groups(dev, drv->dev_groups);
dev_groups_failed:
	if (dev->bus->remove)
		dev->bus->remove(dev);
	else if (drv->remove)
		drv->remove(dev);
probe_failed:
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_DRIVER_NOT_BOUND, dev);
pinctrl_bind_failed:
	device_links_no_driver(dev);
	devres_release_all(dev);
	arch_teardown_dma_ops(dev);
	kfree(dev->dma_range_map);
	dev->dma_range_map = NULL;
	driver_sysfs_remove(dev);
	dev->driver = NULL;
	dev_set_drvdata(dev, NULL);
	if (dev->pm_domain && dev->pm_domain->dismiss)
		dev->pm_domain->dismiss(dev);
	pm_runtime_reinit(dev);
	dev_pm_set_driver_flags(dev, 0);

	switch (ret) {
	case -EPROBE_DEFER:
		/* Driver requested deferred probing */
		dev_dbg(dev, "Driver %s requests probe deferral\n", drv->name);
		driver_deferred_probe_add_trigger(dev, local_trigger_count);
		break;
	case -ENODEV:
	case -ENXIO:
		pr_debug("%s: probe of %s rejects match %d\n",
			 drv->name, dev_name(dev), ret);
		break;
	default:
		/* driver matched but the probe failed */
		pr_warn("%s: probe of %s failed with error %d\n",
			drv->name, dev_name(dev), ret);
	}
	/*
	 * Ignore errors returned by ->probe so that the next driver can try
	 * its luck.
	 */
	ret = 0;
done:
	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE really exit ret=%d bound=%d", ret,
			!!dev->driver);
	if (a52_rscc_probe_device(dev))
		a52_ackfr_record("RSCCCORE really-probe done dev=%s rc=%d bound=%s",
			dev_name(dev), ret, dev->driver && dev->driver->name ?
			dev->driver->name : "-");
	if (a52_display_probe_device(dev))
		a52_ackfr_record("DISP RP done dev=%s rc=%d bound=%s",
			dev_name(dev), ret, dev->driver && dev->driver->name ?
			dev->driver->name : "-");
	atomic_dec(&probe_count);
	wake_up_all(&probe_waitqueue);
	if (a52_storage_probe_device(dev)) {
		do { } while (0);
		do { } while (0);
		do { } while (0);
	}
	return a52_g238_dd_return(dev, (ret), __LINE__);
}

/*
 * For initcall_debug, show the driver probe time.
 */
static int really_probe_debug(struct device *dev, struct device_driver *drv)
{
	ktime_t calltime, rettime;
	int ret;

	calltime = ktime_get();
	ret = really_probe(dev, drv);
	rettime = ktime_get();
	/*
	 * Don't change this to pr_debug() because that requires
	 * CONFIG_DYNAMIC_DEBUG and we want a simple 'initcall_debug' on the
	 * kernel commandline to print this all the time at the debug level.
	 */
	printk(KERN_DEBUG "probe of %s returned %d after %lld usecs\n",
		 dev_name(dev), ret, ktime_us_delta(rettime, calltime));
	return ret;
}

/**
 * driver_probe_done
 * Determine if the probe sequence is finished or not.
 *
 * Should somehow figure out how to use a semaphore, not an atomic variable...
 */
int driver_probe_done(void)
{
	int local_probe_count = atomic_read(&probe_count);

	pr_debug("%s: probe_count = %d\n", __func__, local_probe_count);
	if (local_probe_count)
		return -EBUSY;
	return 0;
}

/**
 * wait_for_device_probe
 * Wait for device probing to be completed.
 */
void wait_for_device_probe(void)
{
	/* wait for the deferred probe workqueue to finish */
	flush_work(&deferred_probe_work);

	/* wait for the known devices to complete their probing */
	wait_event(probe_waitqueue, atomic_read(&probe_count) == 0);
	async_synchronize_full();
}
EXPORT_SYMBOL_GPL(wait_for_device_probe);

/**
 * driver_probe_device - attempt to bind device & driver together
 * @drv: driver to bind a device to
 * @dev: device to try to bind to the driver
 *
 * This function returns -ENODEV if the device is not registered,
 * 1 if the device is bound successfully and 0 otherwise.
 *
 * This function must be called with @dev lock held.  When called for a
 * USB interface, @dev->parent lock must be held as well.
 *
 * If the device has a parent, runtime-resume the parent before driver probing.
 */
int driver_probe_device(struct device_driver *drv, struct device *dev)
{
	int ret = 0;

	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("dp-in d=%.24s r=%.16s reg=%d cur=%.16s",
			dev_name(dev), drv->name, device_is_registered(dev),
			dev->driver && dev->driver->name ? dev->driver->name : "-");

	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE probe-device enter drv=%s reg=%d",
			drv->name, device_is_registered(dev));

	if (a52_rscc_probe_device(dev))
		a52_ackfr_record("RSCCCORE driver-probe enter dev=%s drv=%s",
			dev_name(dev), drv && drv->name ? drv->name : "-");

	if (!device_is_registered(dev))
		return -ENODEV;

	pr_debug("bus: '%s': %s: matched device %s with driver %s\n",
		 drv->bus->name, __func__, dev_name(dev), drv->name);

	pm_runtime_get_suppliers(dev);
	if (dev->parent)
		pm_runtime_get_sync(dev->parent);

	pm_runtime_barrier(dev);
	if (initcall_debug)
		ret = really_probe_debug(dev, drv);
	else
		ret = really_probe(dev, drv);
	pm_request_idle(dev);

	if (dev->parent)
		pm_runtime_put(dev->parent);

	pm_runtime_put_suppliers(dev);
	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE probe-device exit drv=%s ret=%d",
			drv->name, ret);
	if (a52_rscc_probe_device(dev))
		a52_ackfr_record("RSCCCORE driver-probe exit dev=%s rc=%d bound=%s",
			dev_name(dev), ret, dev->driver && dev->driver->name ?
			dev->driver->name : "-");
	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("dp-out rc=%d bound=%.16s reason=%.28s", ret,
			dev->driver && dev->driver->name ? dev->driver->name : "-",
			a52_r230_reason(dev));
	return ret;
}

static inline bool cmdline_requested_async_probing(const char *drv_name)
{
	return parse_option_str(async_probe_drv_names, drv_name);
}

/* The option format is "driver_async_probe=drv_name1,drv_name2,..." */
static int __init save_async_options(char *buf)
{
	if (strlen(buf) >= ASYNC_DRV_NAMES_MAX_LEN)
		pr_warn("Too long list of driver names for 'driver_async_probe'!\n");

	strlcpy(async_probe_drv_names, buf, ASYNC_DRV_NAMES_MAX_LEN);
	return 1;
}
__setup("driver_async_probe=", save_async_options);

bool driver_allows_async_probing(struct device_driver *drv)
{
	switch (drv->probe_type) {
	case PROBE_PREFER_ASYNCHRONOUS:
		return true;

	case PROBE_FORCE_SYNCHRONOUS:
		return false;

	default:
		if (cmdline_requested_async_probing(drv->name))
			return true;

		if (module_requested_async_probing(drv->owner))
			return true;

		return false;
	}
}

struct device_attach_data {
	struct device *dev;

	/*
	 * Indicates whether we are are considering asynchronous probing or
	 * not. Only initial binding after device or driver registration
	 * (including deferral processing) may be done asynchronously, the
	 * rest is always synchronous, as we expect it is being done by
	 * request from userspace.
	 */
	bool check_async;

	/*
	 * Indicates if we are binding synchronous or asynchronous drivers.
	 * When asynchronous probing is enabled we'll execute 2 passes
	 * over drivers: first pass doing synchronous probing and second
	 * doing asynchronous probing (if synchronous did not succeed -
	 * most likely because there was no driver requiring synchronous
	 * probing - and we found asynchronous driver during first pass).
	 * The 2 passes are done because we can't shoot asynchronous
	 * probe for given device and driver from bus_for_each_drv() since
	 * driver pointer is not guaranteed to stay valid once
	 * bus_for_each_drv() iterates to the next driver on the bus.
	 */
	bool want_async;

	/*
	 * We'll set have_async to 'true' if, while scanning for matching
	 * driver, we'll encounter one that requests asynchronous probing.
	 */
	bool have_async;
};

static int __device_attach_driver(struct device_driver *drv, void *_data)
{
	struct device_attach_data *data = _data;
	struct device *dev = data->dev;
	bool async_allowed;
	bool a52_r238_of_match = false;
	int ret;

	if (a52_r238_cx_walk_dev(dev)) {
		a52_r238_of_match = of_driver_match_device(dev, drv);
		if (a52_r238_of_match || a52_r238_cx_walk_drv(drv))
			A52_R238_CXW("cand r=%.24s of=%d",
				drv && drv->name ? drv->name : "-",
				a52_r238_of_match);
	}
	ret = driver_match_device(drv, dev);
	if (a52_r238_cx_walk_dev(dev) &&
	    (a52_r238_of_match || a52_r238_cx_walk_drv(drv)))
		A52_R238_CXW("match r=%.24s rc=%d",
			drv && drv->name ? drv->name : "-", ret);
	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("ad path=dev match=%d ca=%d wa=%d", ret,
			data->check_async, data->want_async);
	if (a52_smmu_unsec_trace_dev(dev) &&
	    !strcmp(drv->name, "msmdrm_smmu"))
		a52_ackfr_record("DCORE match drv=%s rc=%d", drv->name, ret);
	if (a52_rscc_probe_device(dev) &&
	    (ret || a52_rscc_probe_driver(drv)))
		a52_ackfr_record("RSCCFOCUS match path=device-attach dev=%s drv=%s rc=%d",
			dev_name(dev), drv && drv->name ? drv->name : "-", ret);
	if (ret == 0) {
		/* no match */
		return 0;
	} else if (ret == -EPROBE_DEFER) {
		dev_dbg(dev, "Device match requests probe deferral\n");
		driver_deferred_probe_add(dev);
		/*
		 * Device can't match with a driver right now, so don't attempt
		 * to match or bind with other drivers on the bus.
		 */
		return ret;
	} else if (ret < 0) {
		dev_dbg(dev, "Bus failed to match device: %d\n", ret);
		return ret;
	} /* ret > 0 means positive match */

	async_allowed = driver_allows_async_probing(drv);

	if (async_allowed)
		data->have_async = true;

	if (data->check_async && async_allowed != data->want_async)
		return 0;

	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE match probe drv=%s async=%d",
			drv->name, async_allowed);
	ret = driver_probe_device(drv, dev);
	if (a52_r238_cx_walk_dev(dev) &&
	    (a52_r238_of_match || a52_r238_cx_walk_drv(drv)))
		A52_R238_CXW("probe r=%.24s rc=%d",
			drv && drv->name ? drv->name : "-", ret);
	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("ad path=dev probe=%d async=%d", ret, async_allowed);
	return ret;
}

static void __device_attach_async_helper(void *_dev, async_cookie_t cookie)
{
	struct device *dev = _dev;
	struct device_attach_data data = {
		.dev		= dev,
		.check_async	= true,
		.want_async	= true,
	};

	device_lock(dev);

	/*
	 * Check if device has already been removed or claimed. This may
	 * happen with driver loading, device discovery/registration,
	 * and deferred probe processing happens all at once with
	 * multiple threads.
	 */
	if (dev->p->dead || dev->driver)
		goto out_unlock;

	if (dev->parent)
		pm_runtime_get_sync(dev->parent);

	bus_for_each_drv(dev->bus, NULL, &data, __device_attach_driver);
	dev_dbg(dev, "async probe completed\n");

	pm_request_idle(dev);

	if (dev->parent)
		pm_runtime_put(dev->parent);
out_unlock:
	device_unlock(dev);

	put_device(dev);
}

static int __device_attach(struct device *dev, bool allow_async)
{
	int ret = 0;
	bool async = false;

	if (a52_r238_cx_walk_dev(dev))
		A52_R238_CXW("attach-in a=%d dead=%d cur=%.20s",
			allow_async, dev->p ? dev->p->dead : -1,
			dev->driver && dev->driver->name ? dev->driver->name : "-");

	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE attach enter dev=%s async=%d driver=%d",
			dev_name(dev), allow_async, !!dev->driver);
	device_lock(dev);
	if (dev->p->dead) {
		goto out_unlock;
	} else if (dev->driver) {
		if (device_is_bound(dev)) {
			ret = 1;
			goto out_unlock;
		}
		ret = device_bind_driver(dev);
		if (ret == 0)
			ret = 1;
		else {
			dev->driver = NULL;
			ret = 0;
		}
	} else {
		struct device_attach_data data = {
			.dev = dev,
			.check_async = allow_async,
			.want_async = false,
		};

		if (dev->parent)
			pm_runtime_get_sync(dev->parent);

		ret = bus_for_each_drv(dev->bus, NULL, &data,
					__device_attach_driver);
		if (!ret && allow_async && data.have_async) {
			/*
			 * If we could not find appropriate driver
			 * synchronously and we are allowed to do
			 * async probes and there are drivers that
			 * want to probe asynchronously, we'll
			 * try them.
			 */
			dev_dbg(dev, "scheduling asynchronous probe\n");
			get_device(dev);
			async = true;
		} else {
			pm_request_idle(dev);
		}

		if (dev->parent)
			pm_runtime_put(dev->parent);
	}
out_unlock:
	device_unlock(dev);
	if (async)
		async_schedule_dev(__device_attach_async_helper, dev);
	if (a52_smmu_unsec_trace_dev(dev))
		a52_ackfr_record("DCORE attach exit ret=%d driver=%d async=%d",
			ret, !!dev->driver, async);
	if (a52_r238_cx_walk_dev(dev))
		A52_R238_CXW("attach-out rc=%d async=%d cur=%.20s",
			ret, async,
			dev->driver && dev->driver->name ? dev->driver->name : "-");
	return ret;
}

/**
 * device_attach - try to attach device to a driver.
 * @dev: device.
 *
 * Walk the list of drivers that the bus has and call
 * driver_probe_device() for each pair. If a compatible
 * pair is found, break out and return.
 *
 * Returns 1 if the device was bound to a driver;
 * 0 if no matching driver was found;
 * -ENODEV if the device is not registered.
 *
 * When called for a USB interface, @dev->parent lock must be held.
 */
int device_attach(struct device *dev)
{
	return __device_attach(dev, false);
}
EXPORT_SYMBOL_GPL(device_attach);

void device_initial_probe(struct device *dev)
{
	__device_attach(dev, true);
}

/*
 * __device_driver_lock - acquire locks needed to manipulate dev->drv
 * @dev: Device we will update driver info for
 * @parent: Parent device. Needed if the bus requires parent lock
 *
 * This function will take the required locks for manipulating dev->drv.
 * Normally this will just be the @dev lock, but when called for a USB
 * interface, @parent lock will be held as well.
 */
static void __device_driver_lock(struct device *dev, struct device *parent)
{
	if (parent && dev->bus->need_parent_lock)
		device_lock(parent);
	device_lock(dev);
}

/*
 * __device_driver_unlock - release locks needed to manipulate dev->drv
 * @dev: Device we will update driver info for
 * @parent: Parent device. Needed if the bus requires parent lock
 *
 * This function will release the required locks for manipulating dev->drv.
 * Normally this will just be the the @dev lock, but when called for a
 * USB interface, @parent lock will be released as well.
 */
static void __device_driver_unlock(struct device *dev, struct device *parent)
{
	device_unlock(dev);
	if (parent && dev->bus->need_parent_lock)
		device_unlock(parent);
}

/**
 * device_driver_attach - attach a specific driver to a specific device
 * @drv: Driver to attach
 * @dev: Device to attach it to
 *
 * Manually attach driver to a device. Will acquire both @dev lock and
 * @dev->parent lock if needed.
 */
int device_driver_attach(struct device_driver *drv, struct device *dev)
{
	int ret = 0;

	__device_driver_lock(dev, dev->parent);

	/*
	 * If device has been removed or someone has already successfully
	 * bound a driver before us just skip the driver probe call.
	 */
	if (!dev->p->dead && !dev->driver)
		ret = driver_probe_device(drv, dev);

	__device_driver_unlock(dev, dev->parent);

	return ret;
}

static void __driver_attach_async_helper(void *_dev, async_cookie_t cookie)
{
	struct device *dev = _dev;
	struct device_driver *drv;
	int ret = 0;

	__device_driver_lock(dev, dev->parent);

	drv = dev->p->async_driver;

	/*
	 * If device has been removed or someone has already successfully
	 * bound a driver before us just skip the driver probe call.
	 */
	if (!dev->p->dead && !dev->driver)
		ret = driver_probe_device(drv, dev);

	__device_driver_unlock(dev, dev->parent);

	dev_dbg(dev, "driver %s async attach completed: %d\n", drv->name, ret);

	put_device(dev);
}

static int __driver_attach(struct device *dev, void *data)
{
	struct device_driver *drv = data;
	bool async = false;
	int ret;

	/*
	 * Lock device and try to bind to it. We drop the error
	 * here and always return 0, because we need to keep trying
	 * to bind to devices and some drivers will return an error
	 * simply if it didn't support the device.
	 *
	 * driver_probe_device() will spit a warning if there
	 * is an error.
	 */

/* A52_PHASE240_CX_DRIVER_ATTACH_LATCH_V1 */
	ret = driver_match_device(drv, dev);
	if (a52_r238_cx_dd_pair(dev, drv))
		a52_ackfr_record("CXF240 drv-match d=%.20s r=%.20s rc=%d dead=%d cur=%.16s",
			dev_name(dev), drv->name, ret,
			dev->p ? dev->p->dead : -1,
			dev->driver && dev->driver->name ? dev->driver->name : "-");
	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("ad path=drv match=%d dead=%d cur=%.16s", ret,
			dev->p ? dev->p->dead : -1,
			dev->driver && dev->driver->name ? dev->driver->name : "-");
	if (a52_rscc_probe_device(dev) &&
	    (ret || a52_rscc_probe_driver(drv)))
		a52_ackfr_record("RSCCFOCUS match path=driver-attach dev=%s drv=%s rc=%d",
			dev_name(dev), drv && drv->name ? drv->name : "-", ret);
	if (ret == 0) {
		/* no match */
		return 0;
	} else if (ret == -EPROBE_DEFER) {
		dev_dbg(dev, "Device match requests probe deferral\n");
		driver_deferred_probe_add(dev);
		/*
		 * Driver could not match with device, but may match with
		 * another device on the bus.
		 */
		return 0;
	} else if (ret < 0) {
		dev_dbg(dev, "Bus failed to match device: %d\n", ret);
		/*
		 * Driver could not match with device, but may match with
		 * another device on the bus.
		 */
		return 0;
	} /* ret > 0 means positive match */

	if (driver_allows_async_probing(drv)) {
		/*
		 * Instead of probing the device synchronously we will
		 * probe it asynchronously to allow for more parallelism.
		 *
		 * We only take the device lock here in order to guarantee
		 * that the dev->driver and async_driver fields are protected
		 */
		dev_dbg(dev, "probing driver %s asynchronously\n", drv->name);
		device_lock(dev);
		if (!dev->driver) {
			get_device(dev);
			dev->p->async_driver = drv;
			async = true;
		}
		device_unlock(dev);
		if (async)
			async_schedule_dev(__driver_attach_async_helper, dev);
		return 0;
	}

	ret = device_driver_attach(drv, dev);
	if (a52_r238_cx_dd_pair(dev, drv))
		a52_ackfr_record("CXF240 drv-probe d=%.20s r=%.20s rc=%d bound=%.16s",
			dev_name(dev), drv->name, ret,
			dev->driver && dev->driver->name ? dev->driver->name : "-");
	if (a52_r230_gpu_pair(dev, drv))
		A52_R230_DD("ad path=drv probe=%d bound=%.16s", ret,
			dev->driver && dev->driver->name ? dev->driver->name : "-");

	return 0;
}

/**
 * driver_attach - try to bind driver to devices.
 * @drv: driver.
 *
 * Walk the list of devices that the bus has on it and try to
 * match the driver with each one.  If driver_probe_device()
 * returns 0 and the @dev->driver is set, we've found a
 * compatible pair.
 */
int driver_attach(struct device_driver *drv)
{
	int ret;

	if (a52_r238_cx_walk_drv(drv))
		a52_ackfr_record("CXF240 drvwalk-in r=%.24s bus=%.16s",
			drv->name, drv->bus && drv->bus->name ? drv->bus->name : "-");
	if (a52_r230_gpu_drv(drv) || a52_r238_cx_walk_drv(drv))
		A52_R230_DD("walk-in r=%.16s bus=%.16s", drv->name,
			drv->bus && drv->bus->name ? drv->bus->name : "-");
	ret = bus_for_each_dev(drv->bus, NULL, drv, __driver_attach);
	if (a52_r230_gpu_drv(drv) || a52_r238_cx_walk_drv(drv))
		A52_R230_DD("walk-out r=%.16s rc=%d", drv->name, ret);
	if (a52_r238_cx_walk_drv(drv))
		a52_ackfr_record("CXF240 drvwalk-out r=%.24s rc=%d", drv->name, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(driver_attach);

/*
 * __device_release_driver() must be called with @dev lock held.
 * When called for a USB interface, @dev->parent lock must be held as well.
 */
static void __device_release_driver(struct device *dev, struct device *parent)
{
	struct device_driver *drv;

	drv = dev->driver;
	if (drv) {
		pm_runtime_get_sync(dev);

		while (device_links_busy(dev)) {
			__device_driver_unlock(dev, parent);

			device_links_unbind_consumers(dev);

			__device_driver_lock(dev, parent);
			/*
			 * A concurrent invocation of the same function might
			 * have released the driver successfully while this one
			 * was waiting, so check for that.
			 */
			if (dev->driver != drv) {
				pm_runtime_put(dev);
				return;
			}
		}

		driver_sysfs_remove(dev);

		if (dev->bus)
			blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
						     BUS_NOTIFY_UNBIND_DRIVER,
						     dev);

		pm_runtime_put_sync(dev);

		device_remove_file(dev, &dev_attr_state_synced);
		device_remove_groups(dev, drv->dev_groups);

		if (dev->bus && dev->bus->remove)
			dev->bus->remove(dev);
		else if (drv->remove)
			drv->remove(dev);

		devres_release_all(dev);
		arch_teardown_dma_ops(dev);
		kfree(dev->dma_range_map);
		dev->dma_range_map = NULL;
		dev->driver = NULL;
		dev_set_drvdata(dev, NULL);
		if (dev->pm_domain && dev->pm_domain->dismiss)
			dev->pm_domain->dismiss(dev);
		pm_runtime_reinit(dev);
		dev_pm_set_driver_flags(dev, 0);

		device_links_driver_cleanup(dev);

		klist_remove(&dev->p->knode_driver);
		device_pm_check_callbacks(dev);
		if (dev->bus)
			blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
						     BUS_NOTIFY_UNBOUND_DRIVER,
						     dev);

		kobject_uevent(&dev->kobj, KOBJ_UNBIND);
	}
}

void device_release_driver_internal(struct device *dev,
				    struct device_driver *drv,
				    struct device *parent)
{
	__device_driver_lock(dev, parent);

	if (!drv || drv == dev->driver)
		__device_release_driver(dev, parent);

	__device_driver_unlock(dev, parent);
}

/**
 * device_release_driver - manually detach device from driver.
 * @dev: device.
 *
 * Manually detach device from driver.
 * When called for a USB interface, @dev->parent lock must be held.
 *
 * If this function is to be called with @dev->parent lock held, ensure that
 * the device's consumers are unbound in advance or that their locks can be
 * acquired under the @dev->parent lock.
 */
void device_release_driver(struct device *dev)
{
	/*
	 * If anyone calls device_release_driver() recursively from
	 * within their ->remove callback for the same device, they
	 * will deadlock right here.
	 */
	device_release_driver_internal(dev, NULL, NULL);
}
EXPORT_SYMBOL_GPL(device_release_driver);

/**
 * device_driver_detach - detach driver from a specific device
 * @dev: device to detach driver from
 *
 * Detach driver from device. Will acquire both @dev lock and @dev->parent
 * lock if needed.
 */
void device_driver_detach(struct device *dev)
{
	device_release_driver_internal(dev, NULL, dev->parent);
}

/**
 * driver_detach - detach driver from all devices it controls.
 * @drv: driver.
 */
void driver_detach(struct device_driver *drv)
{
	struct device_private *dev_prv;
	struct device *dev;

	if (driver_allows_async_probing(drv))
		async_synchronize_full();

	for (;;) {
		spin_lock(&drv->p->klist_devices.k_lock);
		if (list_empty(&drv->p->klist_devices.k_list)) {
			spin_unlock(&drv->p->klist_devices.k_lock);
			break;
		}
		dev_prv = list_last_entry(&drv->p->klist_devices.k_list,
				     struct device_private,
				     knode_driver.n_node);
		dev = dev_prv->device;
		get_device(dev);
		spin_unlock(&drv->p->klist_devices.k_lock);
		device_release_driver_internal(dev, drv, dev->parent);
		put_device(dev);
	}
}

/* A52_PHASE445_DEFERRED_SNAPSHOT */
int a52_p445_deferred_snapshot(char *buf, u32 stride, u32 max)
{
    struct device_private *p;
    u32 n=0;
    if (!buf || stride < 2U || !max) return -EINVAL;
    mutex_lock(&deferred_probe_mutex);
    list_for_each_entry(p,&deferred_probe_pending_list,deferred_probe) {
        if (n>=max) break;
        strlcpy(buf+n*stride,dev_name(p->device),stride);
        n++;
    }
    mutex_unlock(&deferred_probe_mutex);
    return (int)n;
}
EXPORT_SYMBOL_GPL(a52_p445_deferred_snapshot);
