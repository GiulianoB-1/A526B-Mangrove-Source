// SPDX-License-Identifier: GPL-2.0-only
/* P481 one-shot manual sound-card registration. No calibration or boot changes. */
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/workqueue.h>
#include <linux/p481_audio_diag.h>
#include <sound/soc.h>

extern int p480_kona_machine_init(void);
extern struct bus_type soundwire_type;
static DEFINE_MUTEX(p481_lock);
static bool card_attempted;
static bool mbhc_enable = true;
static bool rpm_forbid;
static bool dapm_idle_off;
static struct kobject *audio_kobj;
static int alive_t;

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

static ssize_t card_enable_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;
	int rc;

	if (kstrtouint(buf, 0, &val) || val != 1)
		return -EINVAL;
	mutex_lock(&p481_lock);
	if (card_attempted) {
		mutex_unlock(&p481_lock);
		return -EBUSY;
	}
	WRITE_ONCE(card_attempted, true);
	mutex_unlock(&p481_lock);

	/* The knobs are now frozen. Resume/hold already-probed devices
	 * before binding the machine. Later probes use the same policy.
	 */
	if (READ_ONCE(rpm_forbid)) {
		bus_for_each_dev(&platform_bus_type, NULL, NULL, p481_hold_existing);
		bus_for_each_dev(&soundwire_type, NULL, NULL, p481_hold_existing);
	}
	rc = p480_kona_machine_init();
	pr_info("P481 CARD_ENABLE rc=%d\n", rc);
	if (rc)
		pr_err("P480 A3c INIT_FAIL stage=machine rc=%d; A3a preserved\n", rc);
	return rc ? rc : count;
}
static struct kobj_attribute card_enable_attr = __ATTR(card_enable, 0200, NULL, card_enable_store);

static struct attribute *audio_attrs[] = {
	&card_enable_attr.attr, &mbhc_enable_attr.attr, &rpm_forbid_attr.attr,
	&dapm_idle_off_attr.attr, NULL,
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
	pr_info("P481 CARD_GATED default=0\n");
	return 0;
}
