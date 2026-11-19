// SPDX-License-Identifier: GPL-2.0-only
/* P477B: register the real CDC pinctrl before codec device_initcalls.
 * No legacy SLIMbus/I2C WCD9xxx IRQ or core is registered.
 */
#include <linux/init.h>
#include <linux/printk.h>
#include <asoc/msm-cdc-pinctrl.h>
static int __init p472_a3b_cdc_pinctrl_init(void)
{
	int rc = msm_cdc_pinctrl_drv_init();

	pr_info("P472 A3b CDC_PINCTRL register rc=%d\n", rc);
	return rc;
}
subsys_initcall(p472_a3b_cdc_pinctrl_init);
