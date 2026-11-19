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
	pr_err("P472 A3a Q6 REGISTER_FAIL rc=%d\n", rc);
	return rc;
}
late_initcall_sync(p472_a3a_start);
