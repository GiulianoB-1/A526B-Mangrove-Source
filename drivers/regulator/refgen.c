// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Kona REFGEN regulator for the Galaxy A52 5G ACK 5.10 port.
 * The stock DTB exposes qcom,refgen-kona-regulator as DSI refgen-supply.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <linux/a52_ack_secure_flight_recorder.h>

#define A52_REFGEN_DISPLAY_SUPPLY_V1
#define REFGEN_REG_PWRDWN_CTRL5          0x80
#define REFGEN_PWRDWN_CTRL5_MASK         BIT(0)
#define REFGEN_PWRDWN_CTRL5_ENABLE       BIT(0)
#define REFGEN_PWRDWN_CTRL5_DISABLE      0

struct refgen {
	struct regulator_desc rdesc;
	struct regulator_dev *rdev;
	void __iomem *addr;
};

/* A52_PHASE445_REFGEN_SNAPSHOT */
static struct refgen *a52_p445_refgen;

static void refgen_masked_writel(u32 val, u32 mask, void __iomem *addr)
{
	u32 reg;

	reg = readl_relaxed(addr);
	reg = (reg & ~mask) | (val & mask);
	writel_relaxed(reg, addr);
}

static int refgen_kona_enable(struct regulator_dev *rdev)
{
	struct refgen *vreg = rdev_get_drvdata(rdev);
	u32 before, after;

	before = readl_relaxed(vreg->addr + REFGEN_REG_PWRDWN_CTRL5);
	refgen_masked_writel(REFGEN_PWRDWN_CTRL5_ENABLE,
			       REFGEN_PWRDWN_CTRL5_MASK,
			       vreg->addr + REFGEN_REG_PWRDWN_CTRL5);
	after = readl_relaxed(vreg->addr + REFGEN_REG_PWRDWN_CTRL5);
	a52_ackfr_record("REFGEN kona_enable raw=0x%x->0x%x enabled=%u",
			  before, after,
			  !!(after & REFGEN_PWRDWN_CTRL5_MASK));
	return 0;
}

static int refgen_kona_disable(struct regulator_dev *rdev)
{
	struct refgen *vreg = rdev_get_drvdata(rdev);
	u32 before, after;

	before = readl_relaxed(vreg->addr + REFGEN_REG_PWRDWN_CTRL5);
	refgen_masked_writel(REFGEN_PWRDWN_CTRL5_DISABLE,
			       REFGEN_PWRDWN_CTRL5_MASK,
			       vreg->addr + REFGEN_REG_PWRDWN_CTRL5);
	after = readl_relaxed(vreg->addr + REFGEN_REG_PWRDWN_CTRL5);
	a52_ackfr_record("REFGEN kona_disable raw=0x%x->0x%x enabled=%u",
			  before, after,
			  !!(after & REFGEN_PWRDWN_CTRL5_MASK));
	return 0;
}

static int refgen_kona_is_enabled(struct regulator_dev *rdev)
{
	struct refgen *vreg = rdev_get_drvdata(rdev);
	u32 raw;
	int enabled;

	raw = readl_relaxed(vreg->addr + REFGEN_REG_PWRDWN_CTRL5);
	enabled = (raw & REFGEN_PWRDWN_CTRL5_MASK) ==
		  REFGEN_PWRDWN_CTRL5_ENABLE;
	a52_ackfr_record("REFGEN kona_state raw=0x%x enabled=%d", raw, enabled);
	return enabled;
}

static const struct regulator_ops refgen_kona_ops = {
	.enable = refgen_kona_enable,
	.disable = refgen_kona_disable,
	.is_enabled = refgen_kona_is_enabled,
};

static const struct of_device_id refgen_match_table[] = {
	{ .compatible = "qcom,refgen-kona-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, refgen_match_table);

static int refgen_probe(struct platform_device *pdev)
{
	struct regulator_config config = { };
	struct regulator_init_data *init_data;
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	struct resource *res;
	struct refgen *vreg;
	u32 raw;
	int rc;

	match = of_match_device(refgen_match_table, dev);
	a52_ackfr_record("REFGEN probe enter compat=%s",
			  match ? match->compatible : "none");
	if (!match || !dev->of_node) {
		a52_ackfr_record("REFGEN probe fail stage=match rc=%d", -ENODEV);
		return -ENODEV;
	}

	vreg = devm_kzalloc(dev, sizeof(*vreg), GFP_KERNEL);
	if (!vreg) {
		a52_ackfr_record("REFGEN probe fail stage=alloc rc=%d", -ENOMEM);
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || !res->start) {
		a52_ackfr_record("REFGEN probe fail stage=resource rc=%d", -EINVAL);
		return -EINVAL;
	}

	vreg->addr = devm_ioremap_resource(dev, res);
	if (IS_ERR(vreg->addr)) {
		rc = PTR_ERR(vreg->addr);
		a52_ackfr_record("REFGEN probe fail stage=ioremap rc=%d", rc);
		return rc;
	}

	raw = readl_relaxed(vreg->addr + REFGEN_REG_PWRDWN_CTRL5);
	a52_ackfr_record("REFGEN mapped start=0x%llx size=%llu raw=0x%x",
			  (unsigned long long)res->start,
			  (unsigned long long)resource_size(res), raw);

	vreg->rdesc.name = "refgen";
	vreg->rdesc.id = pdev->id;
	vreg->rdesc.owner = THIS_MODULE;
	vreg->rdesc.type = REGULATOR_VOLTAGE;
	vreg->rdesc.ops = &refgen_kona_ops;

	init_data = of_get_regulator_init_data(dev, dev->of_node,
					       &vreg->rdesc);
	if (!init_data) {
		a52_ackfr_record("REFGEN probe fail stage=init_data rc=%d", -ENOMEM);
		return -ENOMEM;
	}
	if (!init_data->constraints.name) {
		a52_ackfr_record("REFGEN probe fail stage=name rc=%d", -EINVAL);
		return -EINVAL;
	}
	if (of_get_property(dev->of_node, "parent-supply", NULL))
		init_data->supply_regulator = "parent";

	config.dev = dev;
	config.init_data = init_data;
	config.driver_data = vreg;
	config.of_node = dev->of_node;
	vreg->rdev = devm_regulator_register(dev, &vreg->rdesc, &config);
	if (IS_ERR(vreg->rdev)) {
		rc = PTR_ERR(vreg->rdev);
		a52_ackfr_record("REFGEN probe fail stage=register rc=%d", rc);
		return rc;
	}

	rc = refgen_kona_is_enabled(vreg->rdev);
	a52_ackfr_record("REFGEN probe ready initial_enabled=%d", rc);
	dev_info(dev, "A52 REFGEN regulator registered, initial state=%d\n", rc);
	a52_p445_refgen=vreg;
	return 0;
}

static struct platform_driver refgen_driver = {
	.probe = refgen_probe,
	.driver = {
		.name = "qcom,refgen-regulator",
		.of_match_table = refgen_match_table,
	},
};

static int __init refgen_init(void)
{
	int rc;

	rc = platform_driver_register(&refgen_driver);
	a52_ackfr_record("REFGEN driver_register rc=%d", rc);
	return rc;
}
arch_initcall(refgen_init);

static void __exit refgen_exit(void)
{
	platform_driver_unregister(&refgen_driver);
}
module_exit(refgen_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm REFGEN regulator with A52 display diagnostics");


int a52_p445_refgen_snapshot(u32 *registered, u32 *enabled,
        u32 *use_count, u32 *pwrdwn)
{
    struct refgen *v=a52_p445_refgen;
    if (registered) *registered=!!v;
    if (!v || !v->rdev || !v->addr) return -ENODEV;
    if (enabled) *enabled=refgen_kona_is_enabled(v->rdev);
    if (use_count) *use_count=READ_ONCE(v->rdev->use_count);
    if (pwrdwn) *pwrdwn=readl_relaxed(v->addr+REFGEN_REG_PWRDWN_CTRL5);
    return 0;
}
EXPORT_SYMBOL_GPL(a52_p445_refgen_snapshot);
