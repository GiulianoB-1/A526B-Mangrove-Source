// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm legacy standalone GDSC regulator bridge for Samsung A52 Lagoon DT.
 *
 * The vendor DT exposes standalone "qcom,gdsc" regulators that Android
 * common 5.10 does not otherwise bind:
 *
 *   - gcc_ufs_phy_gdsc: boot-critical UFS profile, kept on after enable
 *   - mdss_core_gdsc: display profile, normal software collapse supported
 *   - gpu_gx_gdsc: exact Lagoon GX profile with reset and clamp sequencing
 *
 * Keep the profiles explicit. Do not claim unrelated qcom,gdsc nodes.
 *
 * A52_PHASE231_GPU_GX_GDSC_PROVIDER
 * A52_PHASE232_GPU_GX_OPTIONAL_AON
 * A52_PHASE233_FINAL_GRAPHICS_PARITY
 */

#include <linux/a52_ack_secure_flight_recorder.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/of_platform.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/regulator/consumer.h>
#include <dt-bindings/regulator/qcom,rpmh-regulator-levels.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regmap.h>
#include <linux/string.h>

#define A52_GDSC_PWR_ON          BIT(31)
#define A52_GDSC_SW_OVERRIDE     BIT(2)
#define A52_GDSC_HW_CONTROL      BIT(1)
#define A52_GDSC_SW_COLLAPSE     BIT(0)
#define A52_GDSC_TIMEOUT_US      100
#define A52_GDSC_GPU_GX_ADDR      0x03d9100c
#define A52_GDSC_GPU_CX_ADDR      0x03d9106c
#define A52_GDSC_CLK_DIS_WAIT_MASK (0xFU << 12)
#define A52_GDSC_CLK_DIS_WAIT_SHIFT 12
#define A52_GDSC_GMEM_CLAMP_IO    BIT(0)
#define A52_GDSC_GMEM_RESET       BIT(4)
#define A52_GDSC_GPU_SW_RESET     BIT(0)

enum a52_legacy_gdsc_profile {
    A52_GDSC_PROFILE_UFS,
    A52_GDSC_PROFILE_MDSS,
    A52_GDSC_PROFILE_GPU_GX,
    A52_GDSC_PROFILE_GPU_CX,
};

struct a52_legacy_gdsc {
    struct device *dev;
    void __iomem *gdscr;
    struct regulator_desc desc;
    enum a52_legacy_gdsc_profile profile;
    struct regmap *domain_addr;
    struct regmap *sw_reset;
    struct regulator *parent_regulator; /* A52_PHASE239_GPU_CX_VDD_PARENT_V1 */
    bool support_hw_trigger;
    bool reset_aon;
    bool no_status_check_on_disable;
    bool skip_disable_before_enable;
    u32 timeout_us;
};

extern void a52_persistent_diag_mark(const char *fmt, ...);

static const char *a52_legacy_gdsc_profile_name(
        const struct a52_legacy_gdsc *gdsc)
{
    switch (gdsc->profile) {
    case A52_GDSC_PROFILE_MDSS:
        return "mdss";
    case A52_GDSC_PROFILE_GPU_GX:
        return "gpu-gx";
    case A52_GDSC_PROFILE_GPU_CX:
        return "gpu-cx";
    case A52_GDSC_PROFILE_UFS:
    default:
        return "ufs";
    }
}

static int a52_legacy_gdsc_poll(struct a52_legacy_gdsc *gdsc, bool on,
                               u32 *last)
{
    u32 val;
    u32 timeout = gdsc->timeout_us ? gdsc->timeout_us :
                  A52_GDSC_TIMEOUT_US;
    int ret;

    if (on)
        ret = readl_poll_timeout(gdsc->gdscr, val,
                                 val & A52_GDSC_PWR_ON,
                                 1, timeout);
    else
        ret = readl_poll_timeout(gdsc->gdscr, val,
                                 !(val & A52_GDSC_PWR_ON),
                                 1, timeout);
    if (last)
        *last = val;
    return ret;
}

/* A52_GDSC_NO_NESTED_REGULATOR: all GDSC regulator ops are MMIO-only.
 * The regulator core already manages the parent-supply enable ordering.
 * Phase239 vdd_parent minimum-level vote is installed at GPU CX PROBE,
 * outside all regulator ops and all regulator core ww-locks.
 */
static int a52_legacy_gdsc_is_enabled(struct regulator_dev *rdev)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 val;

    if (gdsc->profile == A52_GDSC_PROFILE_GPU_GX &&
        gdsc->skip_disable_before_enable)
        return 0;
    val = readl_relaxed(gdsc->gdscr);
    return !!((val & A52_GDSC_PWR_ON) &&
              !(val & A52_GDSC_SW_COLLAPSE));
}

static int a52_legacy_gdsc_enable(struct regulator_dev *rdev)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 before, val;
    int ret;

    before = readl_relaxed(gdsc->gdscr);
    pr_warn_ratelimited("P446M GDSC enable enter name=%s caller=%pS use=%d before=%08x mode=%s\n",
        gdsc->desc.name, __builtin_return_address(0), rdev->use_count,
        before, (before & A52_GDSC_HW_CONTROL) ? "HW" : "SW");

    /*
     * A52_PHASE415_MDSS_GDSC_HWCTRL_PARITY_V1
     *
     * TouchGrass refuses a software enable while an HW-trigger-capable GDSC
     * is already under HW_CONTROL. Do the same for MDSS only. Clearing
     * HW_CONTROL here would steal ownership from SDE RSC after its FAST-mode
     * handoff.
     */
    if (gdsc->profile == A52_GDSC_PROFILE_MDSS &&
        (before & A52_GDSC_HW_CONTROL)) {
        a52_ackfr_record(
            "P415 GDSC enable-block before=%x hw=1 collapse=%u pwr=%u",
            before, !!(before & A52_GDSC_SW_COLLAPSE),
            !!(before & A52_GDSC_PWR_ON));
        pr_warn_ratelimited("P446M GDSC blocked name=%s use=%d before=%08x rc=%d\n",
            gdsc->desc.name, rdev->use_count, before, -EBUSY);
        return -EBUSY;
    }

    val = before;
    val &= ~(A52_GDSC_HW_CONTROL | A52_GDSC_SW_OVERRIDE |
             A52_GDSC_SW_COLLAPSE);
    writel_relaxed(val, gdsc->gdscr);
    mb();
    udelay(1);

    ret = a52_legacy_gdsc_poll(gdsc, true, &val);
    a52_ackfr_record(
        "A52GDSC enable profile=%s name=%s rc=%d before=0x%x after=0x%x",
        a52_legacy_gdsc_profile_name(gdsc), gdsc->desc.name,
        ret, before, val);
    a52_persistent_diag_mark(
        "A52GDSC ENABLE dev=%s name=%s ret=%d reg=0x%08x\n",
        dev_name(gdsc->dev), gdsc->desc.name, ret, val);

    if (ret) {
        dev_err(gdsc->dev, "enable timed out, GDSCR=0x%08x\n", val);
    }
    pr_warn_ratelimited("P446M GDSC enable exit name=%s use=%d rc=%d after=%08x\n",
        gdsc->desc.name, rdev->use_count, ret,
        readl_relaxed(gdsc->gdscr));
    return ret;
}

static int a52_legacy_gdsc_regmap_bit(struct regmap *map, u32 mask,
                                     bool set, u32 *last)
{
    u32 val;
    int ret;

    ret = regmap_read(map, 0, &val);
    if (ret)
        return ret;

    if (set)
        val |= mask;
    else
        val &= ~mask;

    ret = regmap_write(map, 0, val);
    if (ret)
        return ret;

    ret = regmap_read(map, 0, &val);
    if (!ret && last)
        *last = val;
    return ret;
}

static int a52_legacy_gdsc_enable_gpu_gx(struct regulator_dev *rdev)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 before, val, reset = 0, domain = 0;
    int ret;

    before = readl_relaxed(gdsc->gdscr);

    if (gdsc->skip_disable_before_enable) {
        a52_ackfr_record(
            "A52GDSC GPU_GX_SKIP_ENABLE_V1 name=%s reg=0x%x",
            gdsc->desc.name, before);
        a52_persistent_diag_mark(
            "A52GDSC GPU_GX_SKIP_ENABLE dev=%s name=%s reg=0x%08x\\n",
            dev_name(gdsc->dev), gdsc->desc.name, before);
        return 0;
    }

    ret = a52_legacy_gdsc_regmap_bit(gdsc->sw_reset,
                                     A52_GDSC_GPU_SW_RESET, true, &reset);
    if (ret)
        goto out;
    udelay(1);
    ret = a52_legacy_gdsc_regmap_bit(gdsc->sw_reset,
                                     A52_GDSC_GPU_SW_RESET, false, &reset);
    if (ret)
        goto out;

    if (gdsc->reset_aon) {
        ret = a52_legacy_gdsc_regmap_bit(gdsc->domain_addr,
                                         A52_GDSC_GMEM_RESET, true,
                                         &domain);
        if (ret)
            goto out;
        udelay(1);
        ret = a52_legacy_gdsc_regmap_bit(gdsc->domain_addr,
                                         A52_GDSC_GMEM_RESET, false,
                                         &domain);
        if (ret)
            goto out;
    }

    ret = a52_legacy_gdsc_regmap_bit(gdsc->domain_addr,
                                     A52_GDSC_GMEM_CLAMP_IO, false,
                                     &domain);
    if (ret)
        goto out;

    val = readl_relaxed(gdsc->gdscr);
    val &= ~(A52_GDSC_HW_CONTROL | A52_GDSC_SW_OVERRIDE |
             A52_GDSC_SW_COLLAPSE);
    writel_relaxed(val, gdsc->gdscr);
    mb();
    udelay(1);

    ret = a52_legacy_gdsc_poll(gdsc, true, &val);
out:
    if (ret)
        val = readl_relaxed(gdsc->gdscr);
    a52_ackfr_record(
        "A52GDSC gpu-enable rc=%d before=0x%x after=0x%x reset=0x%x domain=0x%x",
        ret, before, val, reset, domain);
    a52_persistent_diag_mark(
        "A52GDSC GPU_ENABLE dev=%s ret=%d reg=0x%08x reset=0x%08x domain=0x%08x\n",
        dev_name(gdsc->dev), ret, val, reset, domain);
    if (ret)
        dev_err(gdsc->dev, "GPU GX enable failed: %d, GDSCR=0x%08x\n",
                ret, val);
    return ret;
}

static int a52_legacy_gdsc_disable_gpu_gx(struct regulator_dev *rdev)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 before, val, domain = 0;
    int ret;

    before = readl_relaxed(gdsc->gdscr);
    val = before;
    if (val & A52_GDSC_HW_CONTROL)
        val &= ~A52_GDSC_HW_CONTROL;
    val |= A52_GDSC_SW_COLLAPSE;
    writel_relaxed(val, gdsc->gdscr);
    mb();
    udelay(1);

    ret = a52_legacy_gdsc_poll(gdsc, false, &val);
    if (!ret)
        ret = a52_legacy_gdsc_regmap_bit(gdsc->domain_addr,
                                         A52_GDSC_GMEM_CLAMP_IO, true,
                                         &domain);

    a52_ackfr_record(
        "A52GDSC gpu-disable rc=%d before=0x%x after=0x%x domain=0x%x",
        ret, before, val, domain);
    if (ret)
        dev_err(gdsc->dev, "GPU GX disable failed: %d, GDSCR=0x%08x\n",
                ret, val);
    return ret;
}

static int a52_legacy_gdsc_disable_gpu_cx(struct regulator_dev *rdev)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 before, val;
    int ret = 0;

    before = readl_relaxed(gdsc->gdscr);
    val = before;
    if (val & A52_GDSC_HW_CONTROL)
        val &= ~A52_GDSC_HW_CONTROL;
    val |= A52_GDSC_SW_COLLAPSE;
    writel_relaxed(val, gdsc->gdscr);
    mb();
    udelay(1);

    if (!gdsc->no_status_check_on_disable)
        ret = a52_legacy_gdsc_poll(gdsc, false, &val);
    else
        val = readl_relaxed(gdsc->gdscr);

    a52_ackfr_record(
        "A52GDSC gpu-cx-disable rc=%d before=0x%x after=0x%x no-check=%u",
        ret, before, val, gdsc->no_status_check_on_disable);
    a52_persistent_diag_mark(
        "A52GDSC GPU_CX_DISABLE dev=%s ret=%d reg=0x%08x no_check=%u\n",
        dev_name(gdsc->dev), ret, val,
        gdsc->no_status_check_on_disable);
    if (ret)
        dev_err(gdsc->dev, "GPU CX disable failed: %d, GDSCR=0x%08x\n",
                ret, val);
    return ret;
}

static int a52_legacy_gdsc_disable_ufs(struct regulator_dev *rdev)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 val = readl_relaxed(gdsc->gdscr);

    a52_ackfr_record(
        "A52GDSC disable-keep-on profile=ufs name=%s reg=0x%x",
        gdsc->desc.name, val);
    a52_persistent_diag_mark(
        "A52GDSC DISABLE_KEEP_ON dev=%s name=%s reg=0x%08x\n",
        dev_name(gdsc->dev), gdsc->desc.name, val);
    return 0;
}

static int a52_legacy_gdsc_disable_mdss(struct regulator_dev *rdev)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 before, val;
    int ret;

    before = readl_relaxed(gdsc->gdscr);
    pr_warn_ratelimited("P446M GDSC disable enter name=%s caller=%pS use=%d before=%08x\n",
        gdsc->desc.name, __builtin_return_address(0),
        rdev->use_count, before);
    val = before;

    /*
     * A52_PHASE415_MDSS_GDSC_HWCTRL_PARITY_V1
     *
     * TouchGrass gdsc_disable() asserts SW_COLLAPSE without clearing
     * HW_CONTROL. SDE RSC deliberately calls regulator_set_mode(FAST)
     * before its first mode-2 entry, then drops its software regulator vote.
     * Preserve that HW ownership across the disable handoff.
     */
    a52_ackfr_record(
        "P415 GDSC disable-pre before=%x hw=%u collapse=%u pwr=%u",
        before, !!(before & A52_GDSC_HW_CONTROL),
        !!(before & A52_GDSC_SW_COLLAPSE),
        !!(before & A52_GDSC_PWR_ON));

    val |= A52_GDSC_SW_COLLAPSE;
    writel_relaxed(val, gdsc->gdscr);
    mb();
    udelay(1);

    ret = a52_legacy_gdsc_poll(gdsc, false, &val);
    a52_ackfr_record(
        "A52GDSC disable profile=mdss name=%s rc=%d before=0x%x after=0x%x",
        gdsc->desc.name, ret, before, val);
    a52_ackfr_record(
        "P415 GDSC disable-post rc=%d after=%x hw=%u collapse=%u pwr=%u",
        ret, val, !!(val & A52_GDSC_HW_CONTROL),
        !!(val & A52_GDSC_SW_COLLAPSE),
        !!(val & A52_GDSC_PWR_ON));
    if (ret)
        dev_err(gdsc->dev, "disable timed out, GDSCR=0x%08x\n", val);
    pr_warn_ratelimited("P446M GDSC disable exit name=%s use=%d rc=%d after=%08x\n",
        gdsc->desc.name, rdev->use_count, ret,
        readl_relaxed(gdsc->gdscr));
    return ret;
}

static unsigned int a52_legacy_gdsc_get_mode(struct regulator_dev *rdev)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 val = readl_relaxed(gdsc->gdscr);

    return val & A52_GDSC_HW_CONTROL ?
           REGULATOR_MODE_FAST : REGULATOR_MODE_NORMAL;
}

static int a52_legacy_gdsc_set_mode(struct regulator_dev *rdev,
                                    unsigned int mode)
{
    struct a52_legacy_gdsc *gdsc = rdev_get_drvdata(rdev);
    u32 before, val;
    int ret = 0;

    if (!gdsc->support_hw_trigger)
        return -EINVAL;

    before = readl_relaxed(gdsc->gdscr);
    pr_warn_ratelimited("P446M GDSC mode enter name=%s caller=%pS use=%d mode=%u before=%08x\n",
        gdsc->desc.name, __builtin_return_address(0),
        rdev->use_count, mode, before);
    val = before;
    switch (mode) {
    case REGULATOR_MODE_FAST:
        val |= A52_GDSC_HW_CONTROL;
        writel_relaxed(val, gdsc->gdscr);
        mb();
        udelay(1);
        break;
    case REGULATOR_MODE_NORMAL:
        val &= ~A52_GDSC_HW_CONTROL;
        writel_relaxed(val, gdsc->gdscr);
        mb();
        udelay(1);
        if ((val & A52_GDSC_PWR_ON) &&
            !(val & A52_GDSC_SW_COLLAPSE))
            ret = a52_legacy_gdsc_poll(gdsc, true, &val);
        break;
    default:
        return -EINVAL;
    }

    a52_ackfr_record(
        "A52GDSC mode profile=%s name=%s mode=%u rc=%d before=0x%x after=0x%x",
        a52_legacy_gdsc_profile_name(gdsc), gdsc->desc.name,
        mode, ret, before, val);
    if (gdsc->profile == A52_GDSC_PROFILE_MDSS)
        a52_ackfr_record(
            "P415 GDSC mode mode=%u rc=%d before=%x after=%x hw=%u pwr=%u",
            mode, ret, before, val, !!(val & A52_GDSC_HW_CONTROL),
            !!(val & A52_GDSC_PWR_ON));
    pr_warn_ratelimited("P446M GDSC mode exit name=%s use=%d mode=%u rc=%d after=%08x\n",
        gdsc->desc.name, rdev->use_count, mode, ret,
        readl_relaxed(gdsc->gdscr));
    return ret;
}

static const struct regulator_ops a52_legacy_gdsc_ufs_ops = {
    .enable = a52_legacy_gdsc_enable,
    .disable = a52_legacy_gdsc_disable_ufs,
    .is_enabled = a52_legacy_gdsc_is_enabled,
};

static const struct regulator_ops a52_legacy_gdsc_mdss_ops = {
    .enable = a52_legacy_gdsc_enable,
    .disable = a52_legacy_gdsc_disable_mdss,
    .is_enabled = a52_legacy_gdsc_is_enabled,
    .set_mode = a52_legacy_gdsc_set_mode,
    .get_mode = a52_legacy_gdsc_get_mode,
};

static const struct regulator_ops a52_legacy_gdsc_gpu_gx_ops = {
    .enable = a52_legacy_gdsc_enable_gpu_gx,
    .disable = a52_legacy_gdsc_disable_gpu_gx,
    .is_enabled = a52_legacy_gdsc_is_enabled,
};

static const struct regulator_ops a52_legacy_gdsc_gpu_cx_ops = {
    .enable = a52_legacy_gdsc_enable,
    .disable = a52_legacy_gdsc_disable_gpu_cx,
    .is_enabled = a52_legacy_gdsc_is_enabled,
};


/* A52_PHASE238_GDSC_PROVIDER_TRACE_V1 */
/* A52_PHASE238_CONTROLFLOW_SAFE_V1 */
#define A52_G238_GDSC_PROP_LIMIT 40
static atomic_t a52_g238_gd_probe_count = ATOMIC_INIT(0);
static int a52_g238_gd_last_rc = -9999;
static int a52_g238_gd_last_stage;
static unsigned long a52_g238_gd_res_start;
static unsigned long a52_g238_gd_res_end;
static u32 a52_g238_gd_wait;
static u32 a52_g238_gd_timeout;
static int a52_g238_gd_wait_rc = -9999;
static int a52_g238_gd_timeout_rc = -9999;
static bool a52_g238_gd_nostatus;
static bool a52_g238_gd_parent_prop;

static bool a52_g238_gd_is_cx(struct platform_device *pdev, const char **rname)
{
	const char *name = NULL;

	if (pdev->dev.of_node)
		of_property_read_string(pdev->dev.of_node, "regulator-name", &name);
	if (rname)
		*rname = name;
	return (name && !strcmp(name, "gpu_cx_gdsc")) ||
	       strstr(dev_name(&pdev->dev), "3d9106c");
}

static bool a52_g238_gd_focus(struct platform_device *pdev, const char **rname)
{
	const char *name = NULL;

	if (pdev->dev.of_node)
		of_property_read_string(pdev->dev.of_node, "regulator-name", &name);
	if (rname)
		*rname = name;
	return (name && (!strcmp(name, "gpu_cx_gdsc") ||
			 !strcmp(name, "gpu_gx_gdsc"))) ||
	       strstr(dev_name(&pdev->dev), "3d9106c") ||
	       strstr(dev_name(&pdev->dev), "3d9100c");
}

static void a52_g238_gd_phandle(struct platform_device *pdev, const char *prop)
{
	struct device_node *np;
	struct platform_device *spdev;

	if (!pdev->dev.of_node)
		return;
	np = of_parse_phandle(pdev->dev.of_node, prop, 0);
	if (!np) {
		a52_ackfr_record("G238 GD ph dev=%s p=%s node=-",
				 dev_name(&pdev->dev), prop);
		return;
	}
	spdev = of_find_device_by_node(np);
	a52_ackfr_record("G238 GD ph dev=%s p=%s node=%s pdrv=%s",
			 dev_name(&pdev->dev), prop, np->full_name,
			 spdev && spdev->dev.driver ? spdev->dev.driver->name : "-");
	if (spdev)
		put_device(&spdev->dev);
	of_node_put(np);
}

static int a52_g238_gd_enter(struct platform_device *pdev)
{
	const char *rname = NULL;
	const char *compat = NULL;
	const struct property *prop;
	struct resource *res;
	u32 val = 0;
	int n = 0;
	bool focus;
	bool cx;

	focus = a52_g238_gd_focus(pdev, &rname);
	if (!focus)
		return 0;
	cx = a52_g238_gd_is_cx(pdev, NULL);
	if (cx)
		atomic_inc(&a52_g238_gd_probe_count);

	if (pdev->dev.of_node)
		compat = of_get_property(pdev->dev.of_node, "compatible", NULL);
	a52_ackfr_record("G238 GD in dev=%s drv=%s rn=%s comp=%s cx=%u",
			 dev_name(&pdev->dev),
			 pdev->dev.driver ? pdev->dev.driver->name : "-",
			 rname ? rname : "-", compat ? compat : "-", cx);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res)
		a52_ackfr_record("G238 GD res dev=%s ok=1 s=%llx e=%llx",
				 dev_name(&pdev->dev),
				 (unsigned long long)res->start,
				 (unsigned long long)res->end);
	else
		a52_ackfr_record("G238 GD res dev=%s ok=0", dev_name(&pdev->dev));
	if (cx && res) {
		WRITE_ONCE(a52_g238_gd_res_start, (unsigned long)res->start);
		WRITE_ONCE(a52_g238_gd_res_end, (unsigned long)res->end);
	}

	if (pdev->dev.of_node) {
		int rc_wait = of_property_read_u32(pdev->dev.of_node,
						   "qcom,clk-dis-wait-val", &val);
		a52_ackfr_record("G238 GD prop dev=%s wait-rc=%d wait=%u",
				 dev_name(&pdev->dev), rc_wait, val);
		if (cx) {
			WRITE_ONCE(a52_g238_gd_wait_rc, rc_wait);
			WRITE_ONCE(a52_g238_gd_wait, val);
		}

		val = 0;
		{
			int rc_timeout = of_property_read_u32(pdev->dev.of_node,
							      "qcom,gds-timeout", &val);
			a52_ackfr_record("G238 GD prop dev=%s timeout-rc=%d timeout=%u",
					 dev_name(&pdev->dev), rc_timeout, val);
			if (cx) {
				WRITE_ONCE(a52_g238_gd_timeout_rc, rc_timeout);
				WRITE_ONCE(a52_g238_gd_timeout, val);
			}
		}
		a52_ackfr_record("G238 GD prop dev=%s nostatus=%u parent=%u hwctl=%u hwctrl=%u",
				 dev_name(&pdev->dev),
				 of_property_read_bool(pdev->dev.of_node,
					"qcom,no-status-check-on-disable"),
				 of_find_property(pdev->dev.of_node,
					"parent-supply", NULL) != NULL,
				 of_find_property(pdev->dev.of_node,
					"hw-ctl-addr", NULL) != NULL,
				 of_find_property(pdev->dev.of_node,
					"hw-ctrl-addr", NULL) != NULL);
		if (cx) {
			WRITE_ONCE(a52_g238_gd_nostatus,
				of_property_read_bool(pdev->dev.of_node,
					"qcom,no-status-check-on-disable"));
			WRITE_ONCE(a52_g238_gd_parent_prop,
				of_find_property(pdev->dev.of_node,
					"parent-supply", NULL) != NULL);
		}

		for_each_property_of_node(pdev->dev.of_node, prop) {
			if (++n > A52_G238_GDSC_PROP_LIMIT) {
				a52_ackfr_record("G238 GD plist dev=%s limit=%d",
						 dev_name(&pdev->dev), n);
				break;
			}
			a52_ackfr_record("G238 GD p dev=%s n=%s",
					 dev_name(&pdev->dev), prop->name);
		}
	}

	a52_g238_gd_phandle(pdev, "parent-supply");
	a52_g238_gd_phandle(pdev, "vdd_parent-supply");
	a52_g238_gd_phandle(pdev, "hw-ctl-addr");
	a52_g238_gd_phandle(pdev, "hw-ctrl-addr");
	return 0;
}

static void a52_g238_gd_stage(struct platform_device *pdev, int stage,
			      const char *op, int line)
{
	if (!a52_g238_gd_focus(pdev, NULL))
		return;
	if (a52_g238_gd_is_cx(pdev, NULL))
		WRITE_ONCE(a52_g238_gd_last_stage, stage);
	a52_ackfr_record("G238 GD st dev=%s s=%d op=%s l=%d",
			 dev_name(&pdev->dev), stage, op, line);
}

static int a52_g238_gd_return(struct platform_device *pdev, int rc, int line)
{
	if (a52_g238_gd_focus(pdev, NULL)) {
		if (a52_g238_gd_is_cx(pdev, NULL))
			WRITE_ONCE(a52_g238_gd_last_rc, rc);
		a52_ackfr_record("G238 GD out dev=%s rc=%d l=%d drv=%s",
				 dev_name(&pdev->dev), rc, line,
				 pdev->dev.driver ? pdev->dev.driver->name : "-");
	}
	return rc;
}

static void a52_g238_gd_replay_workfn(struct work_struct *work)
{
	a52_ackfr_record("G238 RP gd cx-probes=%d stage=%d rc=%d",
			 atomic_read(&a52_g238_gd_probe_count),
			 READ_ONCE(a52_g238_gd_last_stage),
			 READ_ONCE(a52_g238_gd_last_rc));
	a52_ackfr_record("G238 RP gd res=%lx-%lx waitrc=%d wait=%u torc=%d to=%u ns=%u par=%u",
			 READ_ONCE(a52_g238_gd_res_start),
			 READ_ONCE(a52_g238_gd_res_end),
			 READ_ONCE(a52_g238_gd_wait_rc),
			 READ_ONCE(a52_g238_gd_wait),
			 READ_ONCE(a52_g238_gd_timeout_rc),
			 READ_ONCE(a52_g238_gd_timeout),
			 READ_ONCE(a52_g238_gd_nostatus),
			 READ_ONCE(a52_g238_gd_parent_prop));
}

static DECLARE_DELAYED_WORK(a52_g238_gd_replay_work,
			    a52_g238_gd_replay_workfn);

static int __init a52_g238_gd_replay_init(void)
{
	schedule_delayed_work(&a52_g238_gd_replay_work,
			      msecs_to_jiffies(155000));
	return 0;
}
late_initcall(a52_g238_gd_replay_init);

/* A52_PHASE243_CXGX_LIVE_SUPPLIER_V1 */
static void a52_r243_provider3(struct platform_device *pdev)
{
	const char *name = dev_name(&pdev->dev);
	char tag = 0;
	int q;

	if (name && strstr(name, "3d9106c"))
		tag = 'C';
	else if (name && strstr(name, "3d9100c"))
		tag = 'G';
	if (!tag)
		return;
	for (q = 0; q < 3; q++)
		a52_ackfr_record("CXF243 P c=%c q=%d", tag, q);
}

static int a52_legacy_gdsc_probe(struct platform_device *pdev)
{
	int a52_g238_probe_entry __maybe_unused = a52_g238_gd_enter(pdev);
    struct regulator_config config = { };
    struct regulator_init_data *init_data = NULL;
    struct a52_legacy_gdsc *gdsc;
    struct regulator_dev *rdev;
    struct resource *res;
    const char *name;
    u32 before, val;

    if (of_property_read_string(pdev->dev.of_node, "regulator-name", &name))
        return a52_g238_gd_return(pdev, (-EINVAL), __LINE__);

    if (!strcmp(name, "gcc_ufs_phy_gdsc")) {
    } else if (!strcmp(name, "mdss_core_gdsc")) {
    } else if (!strcmp(name, "gpu_gx_gdsc")) {
    } else if (!strcmp(name, "gpu_cx_gdsc")) {
    } else {
        return a52_g238_gd_return(pdev, (-ENODEV), __LINE__);
    }

    gdsc = devm_kzalloc(&pdev->dev, sizeof(*gdsc), GFP_KERNEL);
    if (!gdsc)
        return a52_g238_gd_return(pdev, (-ENOMEM), __LINE__);

    gdsc->dev = &pdev->dev;
    if (!strcmp(name, "mdss_core_gdsc"))
        gdsc->profile = A52_GDSC_PROFILE_MDSS;
    else if (!strcmp(name, "gpu_gx_gdsc"))
        gdsc->profile = A52_GDSC_PROFILE_GPU_GX;
    else if (!strcmp(name, "gpu_cx_gdsc"))
        gdsc->profile = A52_GDSC_PROFILE_GPU_CX;
    else
        gdsc->profile = A52_GDSC_PROFILE_UFS;
    gdsc->support_hw_trigger =
        of_property_read_bool(pdev->dev.of_node,
                              "qcom,support-hw-trigger");

    res = ({ a52_g238_gd_stage(pdev, 120, "resource", __LINE__); platform_get_resource(pdev, IORESOURCE_MEM, 0); });
    if (!res)
        return a52_g238_gd_return(pdev, (-EINVAL), __LINE__);
    if (gdsc->profile == A52_GDSC_PROFILE_GPU_GX &&
        (res->start != A52_GDSC_GPU_GX_ADDR || resource_size(res) != 4)) {
        dev_err(&pdev->dev, "refusing unexpected GPU GX resource\n");
        return a52_g238_gd_return(pdev, (-EINVAL), __LINE__);
    }
    if (gdsc->profile == A52_GDSC_PROFILE_GPU_CX &&
        (res->start != A52_GDSC_GPU_CX_ADDR || resource_size(res) != 4)) {
        dev_err(&pdev->dev, "refusing unexpected GPU CX resource\n");
        return a52_g238_gd_return(pdev, (-EINVAL), __LINE__);
    }
    gdsc->gdscr = devm_ioremap(&pdev->dev, res->start, resource_size(res));
    if (!gdsc->gdscr)
        return a52_g238_gd_return(pdev, (-ENOMEM), __LINE__);

    gdsc->desc.name = name;
    gdsc->desc.of_match = name;
    gdsc->desc.type = REGULATOR_VOLTAGE;
    gdsc->desc.owner = THIS_MODULE;
    switch (gdsc->profile) {
    case A52_GDSC_PROFILE_MDSS:
        gdsc->desc.ops = &a52_legacy_gdsc_mdss_ops;
        break;
    case A52_GDSC_PROFILE_GPU_GX:
        gdsc->desc.ops = &a52_legacy_gdsc_gpu_gx_ops;
        break;
    case A52_GDSC_PROFILE_GPU_CX:
        gdsc->desc.ops = &a52_legacy_gdsc_gpu_cx_ops;
        break;
    case A52_GDSC_PROFILE_UFS:
    default:
        gdsc->desc.ops = &a52_legacy_gdsc_ufs_ops;
        break;
    }

    if (gdsc->profile == A52_GDSC_PROFILE_MDSS) {
        init_data = of_get_regulator_init_data(&pdev->dev,
                                               pdev->dev.of_node,
                                               &gdsc->desc);
        if (init_data && gdsc->support_hw_trigger) {
            init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_MODE;
            init_data->constraints.valid_modes_mask |=
                REGULATOR_MODE_NORMAL | REGULATOR_MODE_FAST;
        }

        before = readl_relaxed(gdsc->gdscr);
        val = before & ~(A52_GDSC_HW_CONTROL | A52_GDSC_SW_OVERRIDE);
        writel_relaxed(val, gdsc->gdscr);
        mb();
        a52_ackfr_record(
            "A52GDSC mdss-init name=%s before=0x%x after=0x%x hw=%u",
            name, before, val, gdsc->support_hw_trigger);
    } else if (gdsc->profile == A52_GDSC_PROFILE_GPU_GX) {
        init_data = of_get_regulator_init_data(&pdev->dev,
                                               pdev->dev.of_node,
                                               &gdsc->desc);
        if (!init_data)
            return a52_g238_gd_return(pdev, (-ENOMEM), __LINE__);
        if (of_get_property(pdev->dev.of_node, "parent-supply", NULL))
            init_data->supply_regulator = "parent";

        gdsc->domain_addr = syscon_regmap_lookup_by_phandle(
                                pdev->dev.of_node, "domain-addr");
        if (IS_ERR(gdsc->domain_addr))
            return a52_g238_gd_return(pdev, (PTR_ERR(gdsc->domain_addr)), __LINE__);
        gdsc->sw_reset = syscon_regmap_lookup_by_phandle(
                             pdev->dev.of_node, "sw-reset");
        if (IS_ERR(gdsc->sw_reset))
            return a52_g238_gd_return(pdev, (PTR_ERR(gdsc->sw_reset)), __LINE__);
        gdsc->reset_aon = of_property_read_bool(pdev->dev.of_node,
                                                "qcom,reset-aon-logic");
        gdsc->skip_disable_before_enable = of_property_read_bool(
            pdev->dev.of_node, "qcom,skip-disable-before-sw-enable");
        /*
         * The Lagoon DT does not set qcom,reset-aon-logic for GPU GX.
         * Match the Qualcomm GDSC helper semantics: the AON/GMEM reset
         * pulse is optional and is executed only when the property exists.
         */

        before = readl_relaxed(gdsc->gdscr);
        val = before & ~(A52_GDSC_HW_CONTROL | A52_GDSC_SW_OVERRIDE);
        writel_relaxed(val, gdsc->gdscr);
        mb();
        a52_ackfr_record(
            "A52GDSC GPU_GX_PROFILE_V2 init name=%s before=0x%x after=0x%x aon=%u",
            name, before, val, gdsc->reset_aon);
        a52_persistent_diag_mark(
            "A52GDSC GPU_PROFILE dev=%s name=%s aon=%u reg=0x%08x\n",
            dev_name(&pdev->dev), name, gdsc->reset_aon, val);
        a52_ackfr_record(
            "A52GDSC GPU_GX_SKIP_PROFILE_V1 name=%s skip=%u",
            name, gdsc->skip_disable_before_enable);
    } else if (gdsc->profile == A52_GDSC_PROFILE_GPU_CX) {
        u32 clk_dis_wait = 0;

        init_data = of_get_regulator_init_data(&pdev->dev,
                                               pdev->dev.of_node,
                                               &gdsc->desc);
        if (!init_data)
            return a52_g238_gd_return(pdev, (-ENOMEM), __LINE__);
        if (of_get_property(pdev->dev.of_node, "parent-supply", NULL))
            init_data->supply_regulator = "parent";

        if (of_find_property(pdev->dev.of_node, "vdd_parent-supply", NULL)) {
            int parent_ret;

            gdsc->parent_regulator = ({ a52_g238_gd_stage(pdev, 110, "devm-reg-get", __LINE__); devm_regulator_get(&pdev->dev,
                                                        "vdd_parent"); });
            if (IS_ERR(gdsc->parent_regulator)) {
                parent_ret = PTR_ERR(gdsc->parent_regulator);
                gdsc->parent_regulator = NULL;
                a52_ackfr_record(
                    "A52GDSC CX_VDD_PARENT_GET_V1 dev=%s rc=%d",
                    dev_name(&pdev->dev), parent_ret);
                if (parent_ret != -EPROBE_DEFER)
                    dev_err(&pdev->dev,
                            "Unable to get vdd_parent regulator: %d\n",
                            parent_ret);
                return a52_g238_gd_return(pdev, (parent_ret), __LINE__);
            }
            /* A52_GDSC_NO_NESTED_REGULATOR: voltage vote outside ops.
             * parent-supply remains core-managed for on/off ordering.
             */
            parent_ret = regulator_set_voltage(gdsc->parent_regulator,
                           RPMH_REGULATOR_LEVEL_LOW_SVS, INT_MAX);
            a52_ackfr_record("P446T CX_PARENT_PROBE_VOTE dev=%s rc=%d level=%d",
                             dev_name(&pdev->dev), parent_ret,
                             RPMH_REGULATOR_LEVEL_LOW_SVS);
            if (parent_ret)
                return a52_g238_gd_return(pdev, (parent_ret), __LINE__);
            a52_ackfr_record(
                "A52GDSC CX_VDD_PARENT_GET_V1 dev=%s rc=0",
                dev_name(&pdev->dev));
        }

        /* A52_PHASE243_GDSC_SUPPLIER_HOOK_V1 */
        a52_r243_provider3(pdev);
        gdsc->timeout_us = A52_GDSC_TIMEOUT_US;
        of_property_read_u32(pdev->dev.of_node, "qcom,gds-timeout",
                             &gdsc->timeout_us);
        gdsc->no_status_check_on_disable = of_property_read_bool(
            pdev->dev.of_node, "qcom,no-status-check-on-disable");

        before = readl_relaxed(gdsc->gdscr);
        val = before & ~(A52_GDSC_HW_CONTROL | A52_GDSC_SW_OVERRIDE);
        if (!of_property_read_u32(pdev->dev.of_node,
                                  "qcom,clk-dis-wait-val",
                                  &clk_dis_wait)) {
            val &= ~A52_GDSC_CLK_DIS_WAIT_MASK;
            val |= (clk_dis_wait << A52_GDSC_CLK_DIS_WAIT_SHIFT) &
                   A52_GDSC_CLK_DIS_WAIT_MASK;
        }
        writel_relaxed(val, gdsc->gdscr);
        mb();
        a52_ackfr_record(
            "A52GDSC GPU_CX_PROFILE_V1 init name=%s before=0x%x after=0x%x wait=%u timeout=%u no-check=%u",
            name, before, val, clk_dis_wait, gdsc->timeout_us,
            gdsc->no_status_check_on_disable);
        a52_persistent_diag_mark(
            "A52GDSC GPU_CX_PROFILE dev=%s name=%s wait=%u timeout=%u no_check=%u reg=0x%08x\n",
            dev_name(&pdev->dev), name, clk_dis_wait, gdsc->timeout_us,
            gdsc->no_status_check_on_disable, val);
    }

    config.dev = &pdev->dev;
    config.of_node = pdev->dev.of_node;
    config.init_data = init_data;
    config.driver_data = gdsc;

    a52_ackfr_record(
        "A52GDSC register enter dev=%s name=%s profile=%s hw=%u",
        dev_name(&pdev->dev), name,
        a52_legacy_gdsc_profile_name(gdsc), gdsc->support_hw_trigger);
    rdev = ({ a52_g238_gd_stage(pdev, 130, "devm-reg-register", __LINE__); devm_regulator_register(&pdev->dev, &gdsc->desc, &config); });
    if (IS_ERR(rdev)) {
        int ret = PTR_ERR(rdev);

        a52_ackfr_record(
            "A52GDSC register exit dev=%s name=%s rc=%d",
            dev_name(&pdev->dev), name, ret);
        if (ret != -EPROBE_DEFER)
            dev_err(&pdev->dev, "failed to register %s: %d\n", name, ret);
        return a52_g238_gd_return(pdev, (ret), __LINE__);
    }

    platform_set_drvdata(pdev, gdsc);
    val = readl_relaxed(gdsc->gdscr);
    a52_ackfr_record(
        "A52GDSC register exit dev=%s name=%s rc=0 reg=0x%x profile=%s",
        dev_name(&pdev->dev), name, val,
        a52_legacy_gdsc_profile_name(gdsc));
    a52_persistent_diag_mark(
        "A52GDSC PROBE dev=%s name=%s reg=0x%08x pwr=%u collapse=%u\n",
        dev_name(&pdev->dev), name, val,
        !!(val & A52_GDSC_PWR_ON), !!(val & A52_GDSC_SW_COLLAPSE));
    dev_info(&pdev->dev, "registered A52 legacy GDSC regulator %s (%s)\n",
             name, a52_legacy_gdsc_profile_name(gdsc));
    return a52_g238_gd_return(pdev, (0), __LINE__);
}

static const struct of_device_id a52_legacy_gdsc_match[] = {
    { .compatible = "qcom,gdsc" },
    { }
};
MODULE_DEVICE_TABLE(of, a52_legacy_gdsc_match);

static struct platform_driver a52_legacy_gdsc_driver = {
    .probe = a52_legacy_gdsc_probe,
    .driver = {
        .name = "a52-legacy-gdsc-regulator",
        .of_match_table = a52_legacy_gdsc_match,
    },
};

static int __init a52_legacy_gdsc_init(void)
{
    int ret;

    a52_ackfr_record("A52GDSC driver-register enter");
    ret = platform_driver_register(&a52_legacy_gdsc_driver);
    a52_ackfr_record("A52GDSC driver-register exit rc=%d", ret);
    return ret;
}
subsys_initcall(a52_legacy_gdsc_init);

static void __exit a52_legacy_gdsc_exit(void)
{
    platform_driver_unregister(&a52_legacy_gdsc_driver);
}
module_exit(a52_legacy_gdsc_exit);

MODULE_DESCRIPTION("Samsung A52 legacy Qualcomm UFS, MDSS, GPU GX and GPU CX GDSC regulator bridge");
MODULE_LICENSE("GPL");

/* A52_PHASE415_MDSS_GDSC_HWCTRL_PARITY_V1 */

/* A52_PHASE446M_GDSC_IRQ_AB_V1 diag */
