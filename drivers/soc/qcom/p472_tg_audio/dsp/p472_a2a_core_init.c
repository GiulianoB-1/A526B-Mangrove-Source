// SPDX-License-Identifier: GPL-2.0-only
/* P472 A2a: donor q6core + cal/real legacy ION, no routing or DAC. */
#include <linux/init.h>
#include <linux/kernel.h>

#ifdef CONFIG_P480_A3C
static bool p472_a2a_init_complete;

bool p472_a2a_ready(void)
{
	return READ_ONCE(p472_a2a_init_complete);
}
#endif

extern int adsp_err_init(void);
extern void adsp_err_exit(void);
extern int audio_cal_init(void);
extern void audio_cal_exit(void);
extern int core_init(void);
extern void core_exit(void);
extern int msm_audio_ion_init(void);
extern void msm_audio_ion_exit(void);

static int __init p472_a2a_init(void)
{
	int rc;

	rc = adsp_err_init();
	if (rc)
		goto fail;
	rc = audio_cal_init();
	if (rc)
		goto err_cal;
	rc = core_init();
	if (rc)
		goto err_core;
	rc = msm_audio_ion_init();
	if (rc)
		goto err_ion;
#ifdef CONFIG_P480_A3C
	/* Only a fully successful A2a (cal/core/ION) permits A3c. */
	WRITE_ONCE(p472_a2a_init_complete, true);
#endif
	pr_info("P472 A2A TG Q6CORE_CAL_ION_LINKED real_ion=1 heaps=0\n");
	return 0;
err_ion:
	core_exit();
err_core:
	audio_cal_exit();
err_cal:
	adsp_err_exit();
fail:
	pr_err("P472 A2A Q6CORE_INIT_FAILED rc=%d\n", rc);
	return rc;
}
late_initcall(p472_a2a_init);
