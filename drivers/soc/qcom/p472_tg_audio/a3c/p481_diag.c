// SPDX-License-Identifier: GPL-2.0-only
/* P483 automatic card registration with ADSP-ready gate and manual recovery. */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <ipc/apr.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/workqueue.h>
#include <linux/p481_audio_diag.h>
#include <linux/p484_audio.h>
#include <sound/soc.h>

extern int p480_kona_machine_init(void);
extern struct bus_type soundwire_type;
static DEFINE_MUTEX(p481_lock);
static bool card_attempted;
static bool card_enable = true;
static unsigned int bolero_guard = 1;
static bool p486_auto_started;
static bool p487_debug;

bool p487_audio_debug(void)
{
	return READ_ONCE(p487_debug);
}

static int __init p487_debug_setup(char *str)
{
	bool enabled;

	if (kstrtobool(str, &enabled))
		return 0;
	p487_debug = enabled;
	return 1;
}
__setup("a52_audio.debug=", p487_debug_setup);

static ssize_t debug_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", p487_audio_debug());
}

static ssize_t debug_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	bool enabled;

	if (kstrtobool(buf, &enabled))
		return -EINVAL;
	WRITE_ONCE(p487_debug, enabled);
	return count;
}
static struct kobj_attribute debug_attr = __ATTR_RW(debug);

unsigned int p483_bolero_guard(void)
{
	return READ_ONCE(bolero_guard);
}

static int __init p483_card_enable_setup(char *str)
{
	bool enabled;

	if (kstrtobool(str, &enabled))
		return 0;
	card_enable = enabled;
	return 1;
}
__setup("a52_audio.card_enable=", p483_card_enable_setup);
static bool mbhc_enable = true;
static bool rpm_forbid;
static bool dapm_idle_off;
static struct kobject *audio_kobj;
static int alive_t;

bool p481_card_armed(void)
{
	return READ_ONCE(card_attempted);
}

bool p481_mbhc_disabled(void)
{
	return !READ_ONCE(mbhc_enable);
}

bool p481_bias_hold(void)
{
	return READ_ONCE(dapm_idle_off);
}

bool p481_kona_card(const struct snd_soc_card *card)
{
	return card && card->dev && card->dev->of_node && card->name &&
		!strcmp(card->name, "lito-lagoonmtp-snd-card") &&
		of_device_is_compatible(card->dev->of_node, "qcom,kona-asoc-snd");
}

static void p481_alive_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(alive_job, p481_alive_work);
static void p481_alive_work(struct work_struct *work)
{
	pr_info("P481 ALIVE t=%d\n", ++alive_t);
	if (alive_t < 15)
		schedule_delayed_work(&alive_job, HZ);
}

void p481_alive_start(void)
{
	alive_t = 0;
	schedule_delayed_work(&alive_job, HZ);
}

/* Only exact live audio device families, not unrelated platform devices. */
static bool p481_audio_device(struct device *dev)
{
	static const char * const compatibles[] = {
		"qcom,bolero-codec", "qcom,va-macro", "qcom,tx-macro",
		"qcom,rx-macro", "qcom,swr-mstr", "qcom,lpi-pinctrl",
		"qcom,wcd938x-codec", "qcom,wcd938x-slave",
	};
	int i;

	if (!dev->of_node)
		return false;
	for (i = 0; i < ARRAY_SIZE(compatibles); i++)
		if (of_device_is_compatible(dev->of_node, compatibles[i]))
			return true;
	return false;
}

void p481_rpm_forbid_device(struct device *dev)
{
	if (!READ_ONCE(card_attempted) || !READ_ONCE(rpm_forbid) ||
	    !p481_audio_device(dev))
		return;
	if (!pm_runtime_enabled(dev)) {
		dev_info(dev, "P481 RPM_NO_PM dev=%s\n", dev_name(dev));
		return;
	}
	pm_runtime_forbid(dev);
	dev_info(dev, "P481 RPM_FORBID dev=%s active=%d\n",
		 dev_name(dev), pm_runtime_active(dev));
}

static int p481_hold_existing(struct device *dev, void *data)
{
	if (dev->driver)
		p481_rpm_forbid_device(dev);
	return 0;
}

#define P481_BOOL_ATTR(name)						\
static ssize_t name##_show(struct kobject *kobj,				\
			  struct kobj_attribute *attr, char *buf)		\
{ return sysfs_emit(buf, "%u\n", READ_ONCE(name)); }			\
static ssize_t name##_store(struct kobject *kobj,				\
			   struct kobj_attribute *attr,				\
			   const char *buf, size_t count)			\
{									\
	unsigned int val;						\
	if (kstrtouint(buf, 0, &val) || val > 1)				\
		return -EINVAL;						\
	mutex_lock(&p481_lock);						\
	if (card_attempted) {						\
		mutex_unlock(&p481_lock);				\
		return -EBUSY;						\
	}								\
	WRITE_ONCE(name, !!val);						\
	mutex_unlock(&p481_lock);					\
	return count;							\
}									\
static struct kobj_attribute name##_attr = __ATTR_RW(name)

P481_BOOL_ATTR(mbhc_enable);
P481_BOOL_ATTR(rpm_forbid);
P481_BOOL_ATTR(dapm_idle_off);

/* Both automatic and manual paths use the same one-shot gate. */
static int p483_register_card(bool manual)
{
	int rc, retry;

	mutex_lock(&p481_lock);
	if (card_attempted) {
		mutex_unlock(&p481_lock);
		return -EBUSY;
	}
	if (!card_enable) {
		mutex_unlock(&p481_lock);
		return -ECANCELED;
	}
	WRITE_ONCE(card_attempted, true);
	mutex_unlock(&p481_lock);

	if (READ_ONCE(rpm_forbid)) {
		bus_for_each_dev(&platform_bus_type, NULL, NULL, p481_hold_existing);
		bus_for_each_dev(&soundwire_type, NULL, NULL, p481_hold_existing);
	}
	/* The work item that claims the locked one-shot is the actual source. */
	pr_info("P483 CARD_AUTO adsp_ready=1\n");
	if (manual)
		pr_info("P486 CARD_AUTO source=manual_knob\n");
	else
		pr_info("P486 CARD_AUTO source=apr_loaded\n");

	/* Never repeat a successful platform_driver_register().
	 * Deferred device probes are already handled by the driver core.
	 * Retry only a transient failure to register the driver itself.
	 */
	for (retry = 0; ; retry++) {
		rc = p480_kona_machine_init();
		if (rc != -EAGAIN && rc != -EPROBE_DEFER)
			break;
		if (retry == 3) {
			pr_err("P487 CARD_RETRY exhausted rc=%d retries=%d\n", rc, retry);
			break;
		}
		pr_warn("P487 CARD_RETRY retry=%d rc=%d delay_ms=2000\n", retry + 1, rc);
		msleep(2000);
		if (apr_get_q6_state() != APR_SUBSYS_LOADED) {
			rc = -ENODEV;
			pr_err("P487 CARD_RETRY abort ADSP not loaded\n");
			break;
		}
	}
	pr_info("P481 CARD_ENABLE rc=%d\n", rc);
	if (rc)
		pr_err("P480 A3c INIT_FAIL stage=machine rc=%d; A3a preserved\n", rc);
	return rc;
}

static void p483_auto_work(struct work_struct *work);
static DECLARE_WORK(p483_card_job, p483_auto_work);
static DECLARE_WORK(p487_manual_card_job, p483_auto_work);

static void p483_auto_work(struct work_struct *work)
{
	if (!READ_ONCE(card_enable) || READ_ONCE(card_attempted))
		return;
	/* APR LOADED is gated by the real transport GET_STATE response. */
	if (apr_get_q6_state() != APR_SUBSYS_LOADED)
		return;
	p483_register_card(work == &p487_manual_card_job);
}

/* Called after the genuine APR UP event; never register in the callback. */
void p486_card_adsp_up(void)
{
	if (READ_ONCE(p486_auto_started) && READ_ONCE(card_enable) &&
	    !READ_ONCE(card_attempted))
		schedule_work(&p483_card_job);
}

void p483_card_auto_start(void)
{
	/* Publish initialization before sampling readiness, so a prior UP
	 * event is caught by the state check and a later one by the hook.
	 */
	WRITE_ONCE(p486_auto_started, true);
	smp_mb();
	if (apr_get_q6_state() == APR_SUBSYS_LOADED)
		p486_card_adsp_up();
}

static ssize_t card_enable_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(card_enable));
}

static ssize_t card_enable_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 0, &val) || val > 1)
		return -EINVAL;
	mutex_lock(&p481_lock);
	if (card_attempted) {
		mutex_unlock(&p481_lock);
		return -EBUSY;
	}
	WRITE_ONCE(card_enable, !!val);
	mutex_unlock(&p481_lock);
	if (val) {
		if (READ_ONCE(p486_auto_started))
			schedule_work(&p487_manual_card_job);
	} else {
		cancel_work_sync(&p483_card_job);
		cancel_work_sync(&p487_manual_card_job);
	}
	return count;
}
static struct kobj_attribute card_enable_attr = __ATTR_RW(card_enable);

static ssize_t bolero_guard_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", p483_bolero_guard());
}

static ssize_t bolero_guard_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 0, &val) || val > 2)
		return -EINVAL;
	WRITE_ONCE(bolero_guard, val);
	pr_info_ratelimited("P483 BOLERO_GUARD mode=%u\n", val);
	return count;
}
static struct kobj_attribute bolero_guard_attr = __ATTR_RW(bolero_guard);

static ssize_t mbhc_dump_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int val;
	int rc;
	if (kstrtouint(buf, 0, &val) || val != 1)
		return -EINVAL;
	rc = p484_mbhc_dump_knob();
	if (rc)
		return rc;
	return count;
}
static struct kobj_attribute mbhc_dump_attr = __ATTR(mbhc_dump, 0200, NULL, mbhc_dump_store);

static struct attribute *audio_attrs[] = {
	&card_enable_attr.attr, &mbhc_enable_attr.attr, &rpm_forbid_attr.attr,
	&dapm_idle_off_attr.attr, &bolero_guard_attr.attr, &mbhc_dump_attr.attr,
	&debug_attr.attr, NULL,
};
static const struct attribute_group audio_group = { .attrs = audio_attrs };

int p481_diag_register(void)
{
	int rc;

	audio_kobj = kobject_create_and_add("a52_audio", kernel_kobj);
	if (!audio_kobj)
		return -ENOMEM;
	rc = sysfs_create_group(audio_kobj, &audio_group);
	if (rc) {
		kobject_put(audio_kobj);
		audio_kobj = NULL;
		return rc;
	}
	if (!READ_ONCE(card_enable))
		pr_info("P481 CARD_GATED default=0\n");
	pr_info("P483 CARD_POLICY auto=%u guard=%u\n",
		READ_ONCE(card_enable), p483_bolero_guard());
	return 0;
}
