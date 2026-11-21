// SPDX-License-Identifier: GPL-2.0-only
/* P472 A3a: real TG services only. No ASoC machine, no speaker routes.
 * TG 4.19 dsp/q6_init.c:10-29 reinitializes q6core/cal/ION.
 * In 5.10 A2a those providers are ALREADY linked and initialized.
 * Defer only version verification, never fabricate a DSP success reply.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <dsp/q6core.h>

extern int rtac_init(void);
extern void rtac_exit(void);
extern int adm_init(void);
extern void adm_exit(void);
extern int afe_init(void);
extern void afe_exit(void);
extern int q6asm_init(void);
extern void q6asm_exit(void);
extern int q6lsm_init(void);
extern void q6lsm_exit(void);

#ifdef CONFIG_P480_A3C
extern bool p472_a2a_ready(void);
extern bool audio_cal_is_initialized(void);
extern int __init p480_audio_platform_init(void);
extern void __init p480_audio_platform_exit(void);
extern int __init p480_kona_machine_init(void);
#endif

static unsigned int p472_a3a_checks;

static void p472_a3a_version_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(p472_a3a_version_job, p472_a3a_version_work);

static void p472_a3a_version_work(struct work_struct *work)
{
	int afe, asm_ver, adm;
	if (!q6core_is_adsp_ready()) {
		if (p472_a3a_checks++ < 9)
			schedule_delayed_work(&p472_a3a_version_job, 10 * HZ);
		else
			pr_err("P472 A3a Q6 VERSION_FAIL adsp_not_ready\n");
		return;
	}
	/* Actual AVCS_GET_FWK_VERSION through the proven TG APR. */
	afe = q6core_get_avcs_api_version_per_service(
			APRV2_IDS_SERVICE_ID_ADSP_AFE_V);
	asm_ver = q6core_get_avcs_api_version_per_service(
			APRV2_IDS_SERVICE_ID_ADSP_ASM_V);
	adm = q6core_get_avcs_api_version_per_service(
			APRV2_IDS_SERVICE_ID_ADSP_ADM_V);
	pr_info("P472 A3a Q6 ADSP_VERSION_REPLY afe=%d asm=%d adm=%d\n",
		afe, asm_ver, adm);
	if (afe != 9 || asm_ver != 2 || adm != 4)
		pr_err("P472 A3a Q6 VERSION_FAIL afe=%d asm=%d adm=%d\n",
		       afe, asm_ver, adm);
}

static int __init p472_a3a_start(void)
{
	int rc;

	/* TG q6_init.c:12-21, but A2a already owns adsp_err/cal/core/ION. */
#ifdef CONFIG_P480_A3C
	if (!p472_a2a_ready() || !audio_cal_is_initialized()) {
		pr_err("P480 A3c PLATFORM_SKIPPED reason=a2a_not_ready\n");
		return -ENODEV;
	}
#endif
	rc = rtac_init();
	if (rc)
		goto failed;
	rc = adm_init();
	if (rc)
		goto undo_rtac;
	pr_info("P472 A3a Q6 adm registered rc=0\n");
	rc = afe_init();
	if (rc)
		goto undo_adm;
	pr_info("P472 A3a Q6 afe registered rc=0\n");
	rc = q6asm_init();
	if (rc)
		goto undo_afe;
	pr_info("P472 A3a Q6 asm registered rc=0\n");
	rc = q6lsm_init();
	if (rc)
		goto undo_asm;
	pr_info("P472 A3a Q6 lsm registered rc=0\n");

#ifdef CONFIG_P480_A3C
	/* Order: calibrated A2a -> A3a services -> A3c platform -> machine.
	 * A3c is non-critical and must never tear down the working Q6 stack.
	 */
	rc = p480_audio_platform_init();
	if (rc) {
		pr_err("P480 A3c INIT_FAIL stage=platform rc=%d; A3a preserved\n", rc);
		pr_err("P480 A3c PLATFORM_SKIPPED reason=platform_register_failed rc=%d; A3a preserved\n", rc);
		goto a3c_nonfatal;
	}
	rc = p480_kona_machine_init();
	if (rc) {
		pr_err("P480 A3c INIT_FAIL stage=machine rc=%d; A3a preserved\n", rc);
		pr_err("P480 A3c MACHINE_SKIPPED reason=machine_register_failed rc=%d; A3a preserved\n", rc);
		p480_audio_platform_exit();
		goto a3c_nonfatal;
	}
	pr_info("P480 A3c INIT_ORDER_PASS A2a=1 A3a=1 platform=1 machine=1\n");
a3c_nonfatal:
#endif

	/* Only log versions returned by the actual ADSP. */
	schedule_delayed_work(&p472_a3a_version_job, 10 * HZ);
	return 0;
undo_asm:
	q6asm_exit();
undo_afe:
	afe_exit();
undo_adm:
	adm_exit();
undo_rtac:
	rtac_exit();
failed:
#ifdef CONFIG_P480_A3C
	pr_err("P480 A3c PLATFORM_SKIPPED reason=a3a_services_failed rc=%d\n", rc);
#endif
	pr_err("P472 A3a Q6 REGISTER_FAIL rc=%d\n", rc);
	return rc;
}
late_initcall_sync(p472_a3a_start);
