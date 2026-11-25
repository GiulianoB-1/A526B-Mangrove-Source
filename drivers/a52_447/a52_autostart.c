// SPDX-License-Identifier: GPL-2.0-only
/* P487: reuse late runtime gates; never enable SCM in the DT. */
#include <linux/a52_autostart.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/pm_wakeup.h>
#include <linux/qcom_scm.h>
#include <linux/remoteproc.h>
#include <linux/workqueue.h>

#if !defined(CONFIG_QCOM_Q6V5_PAS) || !defined(CONFIG_IPA3) || !defined(CONFIG_PM_SLEEP)
#error "P487 autostart requires real built-in PAS, IPA and wakeup support"
#endif

#define P487_MIN_UPTIME_MS 12000
#define P487_DEADLINE_MS 120000
#define P487_POLL_MS 200
static bool autostart = true;
static bool autostart_ipa = true;
static struct wakeup_source *autostart_ws;

bool a52_p487_autostart_enabled(void)
{
	return READ_ONCE(autostart);
}

static int __init autostart_setup(char *str)
{
	return kstrtobool(str, &autostart) ? 0 : 1;
}
__setup("a52.autostart=", autostart_setup);

static int __init autostart_ipa_setup(char *str)
{
	return kstrtobool(str, &autostart_ipa) ? 0 : 1;
}
__setup("a52.autostart_ipa=", autostart_ipa_setup);

static s64 uptime_ms(void)
{
	return ktime_to_ms(ktime_get_boottime());
}

static bool firmware_readable(void)
{
	struct file *file;
	loff_t pos = 0;
	char byte;
	ssize_t ret;

	file = filp_open("/vendor/firmware_mnt/image/adsp.mdt", O_RDONLY, 0);
	if (IS_ERR(file))
		return false;
	ret = kernel_read(file, &byte, 1, &pos);
	filp_close(file, NULL);
	return ret == 1;
}

static void p487_step(const char *step, int rc)
{
	pr_info("P487 AUTOSTART step=%s rc=%d t_ms=%lld\n",
		step, rc, (long long)uptime_ms());
}

static void p487_autostart_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(autostart_job, p487_autostart_work);

static void p487_autostart_work(struct work_struct *work)
{
	struct rproc *rproc = NULL;
	s64 start, deadline;
	int rc, state = -1;
	bool ipa_armed = false;

	if (uptime_ms() >= P487_DEADLINE_MS) {
		pr_err("P487 AUTOSTART timeout waiting for readable firmware\n");
		goto release;
	}
	if (uptime_ms() < P487_MIN_UPTIME_MS || !firmware_readable()) {
		schedule_delayed_work(&autostart_job, msecs_to_jiffies(P487_POLL_MS));
		return;
	}

	start = uptime_ms();
	__pm_stay_awake(autostart_ws);
	rc = a52_p487_scm_enable_once();
	p487_step("scm", rc);
	if (rc && rc != -EALREADY)
		goto done;
	msleep(1000);
	if (!qcom_scm_is_available()) {
		pr_err("P487 AUTOSTART SCM not available after 1s; abort\n");
		goto done;
	}

	if (autostart_ipa) {
		rc = a52_p487_ipa_arm_once(true);
		p487_step("ipa", rc);
		if (rc && rc != -EALREADY)
			goto done;
		ipa_armed = true; /* arm accepted, not IPA READY */
		msleep(1000);
	}

	/* PAS probe is deferred until SCM exists. Wait for the real rproc. */
	deadline = min_t(s64, uptime_ms() + 10000, P487_DEADLINE_MS);
	while (uptime_ms() < deadline) {
		rproc = a52_p487_find_adsp();
		if (rproc)
			break;
		msleep(P487_POLL_MS);
	}
	if (!rproc) {
		p487_step("adsp", -ETIMEDOUT);
		goto done;
	}
	rc = a52_p487_adsp_start_once(rproc);
	/* rproc_boot is synchronous and owns PAS/firmware timeouts.
	 * Never cancel an in-flight secure-world operation.
	 */
	if (!rc || rc == -EALREADY) {
		deadline = uptime_ms() + 5000;
		while (READ_ONCE(rproc->state) != RPROC_RUNNING &&
		       uptime_ms() < deadline)
			msleep(P487_POLL_MS);
		if (READ_ONCE(rproc->state) != RPROC_RUNNING)
			rc = -ETIMEDOUT;
	}
	p487_step("adsp", rc);
	state = READ_ONCE(rproc->state);
	rproc_put(rproc);
done:
	pr_info("P487 AUTOSTART done adsp=%d ipa=%s total_ms=%lld\n",
		state, ipa_armed ? "armed" : "off", (long long)(uptime_ms() - start));
	if (state != RPROC_RUNNING)
		pr_err("P487 AUTOSTART incomplete; no automatic retry\n");
	__pm_relax(autostart_ws);
release:
	wakeup_source_unregister(autostart_ws);
}

static int __init p487_autostart_init(void)
{
	if (!autostart || !of_machine_is_compatible("qcom,lagoon"))
		return 0;
	autostart_ws = wakeup_source_register(NULL, "a52_p487_autostart");
	if (!autostart_ws)
		return -ENOMEM;
	schedule_delayed_work(&autostart_job, msecs_to_jiffies(P487_POLL_MS));
	return 0;
}
late_initcall_sync(p487_autostart_init);
