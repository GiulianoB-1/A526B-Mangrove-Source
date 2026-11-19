// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/clk-provider.h>
/* A52_PHASE11_DOWNSTREAM_CLK_SET_FLAGS_QCOM_HEADER */
#include <linux/clk/qcom.h>
#include <linux/regmap.h>

#include "clk-branch.h"

static bool clk_branch_in_hwcg_mode(const struct clk_branch *br)
{
	u32 val;

	if (!br->hwcg_reg)
		return false;

	regmap_read(br->clkr.regmap, br->hwcg_reg, &val);

	return !!(val & BIT(br->hwcg_bit));
}

static bool clk_branch_check_halt(const struct clk_branch *br, bool enabling)
{
	bool invert = (br->halt_check == BRANCH_HALT_ENABLE);
	u32 val;

	regmap_read(br->clkr.regmap, br->halt_reg, &val);

	val &= BIT(br->halt_bit);
	if (invert)
		val = !val;

	return !!val == !enabling;
}

#define BRANCH_CLK_OFF			BIT(31)
#define BRANCH_NOC_FSM_STATUS_SHIFT	28
#define BRANCH_NOC_FSM_STATUS_MASK	0x7
#define BRANCH_NOC_FSM_STATUS_ON	(0x2 << BRANCH_NOC_FSM_STATUS_SHIFT)

static bool clk_branch2_check_halt(const struct clk_branch *br, bool enabling)
{
	u32 val;
	u32 mask;

	mask = BRANCH_NOC_FSM_STATUS_MASK << BRANCH_NOC_FSM_STATUS_SHIFT;
	mask |= BRANCH_CLK_OFF;

	regmap_read(br->clkr.regmap, br->halt_reg, &val);

	if (enabling) {
		val &= mask;
		return (val & BRANCH_CLK_OFF) == 0 ||
			val == BRANCH_NOC_FSM_STATUS_ON;
	} else {
		return val & BRANCH_CLK_OFF;
	}
}

static int clk_branch_wait(const struct clk_branch *br, bool enabling,
		bool (check_halt)(const struct clk_branch *, bool))
{
	bool voted = br->halt_check & BRANCH_VOTED;
	const char *name = clk_hw_get_name(&br->clkr.hw);

	/*
	 * Skip checking halt bit if we're explicitly ignoring the bit or the
	 * clock is in hardware gated mode
	 */
	if (br->halt_check == BRANCH_HALT_SKIP || clk_branch_in_hwcg_mode(br))
		return 0;

	if (br->halt_check == BRANCH_HALT_DELAY || (!enabling && voted)) {
		udelay(10);
	} else if (br->halt_check == BRANCH_HALT_ENABLE ||
		   br->halt_check == BRANCH_HALT ||
		   (enabling && voted)) {
		int count = 200;

		while (count-- > 0) {
			if (check_halt(br, enabling))
				return 0;
			udelay(1);
		}
		WARN(1, "%s status stuck at 'o%s'", name,
				enabling ? "ff" : "n");
		return -EBUSY;
	}
	return 0;
}

static int clk_branch_toggle(struct clk_hw *hw, bool en,
		bool (check_halt)(const struct clk_branch *, bool))
{
	struct clk_branch *br = to_clk_branch(hw);
	int ret;

	if (en) {
		ret = clk_enable_regmap(hw);
		if (ret)
			return ret;
	} else {
		clk_disable_regmap(hw);
	}

	return clk_branch_wait(br, en, check_halt);
}

static int clk_branch_enable(struct clk_hw *hw)
{
	return clk_branch_toggle(hw, true, clk_branch_check_halt);
}

/* A52_PHASE11_DOWNSTREAM_CLK_SET_FLAGS_QCOM_CBCR */
static int clk_cbcr_set_flags(struct regmap *regmap, unsigned int reg,
			      unsigned long flags)
{
	u32 cbcr_val = 0;
	u32 cbcr_mask;
	int ret;

	switch (flags) {
	case CLKFLAG_PERIPH_OFF_SET:
		cbcr_val = cbcr_mask = BIT(12);
		break;
	case CLKFLAG_PERIPH_OFF_CLEAR:
		cbcr_mask = BIT(12);
		break;
	case CLKFLAG_RETAIN_PERIPH:
		cbcr_val = cbcr_mask = BIT(13);
		break;
	case CLKFLAG_NORETAIN_PERIPH:
		cbcr_mask = BIT(13);
		break;
	case CLKFLAG_RETAIN_MEM:
		cbcr_val = cbcr_mask = BIT(14);
		break;
	case CLKFLAG_NORETAIN_MEM:
		cbcr_mask = BIT(14);
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(regmap, reg, cbcr_mask, cbcr_val);
	if (ret)
		return ret;

	/* Ensure the retention request reaches the CBCR before returning. */
	mb();
	udelay(1);

	return 0;
}

static int clk_branch_set_flags(struct clk_hw *hw, unsigned int flags)
{
	struct clk_branch *br = to_clk_branch(hw);

	return clk_cbcr_set_flags(br->clkr.regmap, br->halt_reg, flags);
}
static void clk_branch_disable(struct clk_hw *hw)
{
	clk_branch_toggle(hw, false, clk_branch_check_halt);
}

const struct clk_ops clk_branch_ops = {
	.enable = clk_branch_enable,
	.disable = clk_branch_disable,
	.is_enabled = clk_is_enabled_regmap,
	/* A52_PHASE11_DOWNSTREAM_CLK_SET_FLAGS_CLK_BRANCH_OPS */
	.set_flags = clk_branch_set_flags,
};
EXPORT_SYMBOL_GPL(clk_branch_ops);

static int clk_branch2_enable(struct clk_hw *hw)
{
	return clk_branch_toggle(hw, true, clk_branch2_check_halt);
}

static void clk_branch2_disable(struct clk_hw *hw)
{
	clk_branch_toggle(hw, false, clk_branch2_check_halt);
}

const struct clk_ops clk_branch2_ops = {
	.enable = clk_branch2_enable,
	.disable = clk_branch2_disable,
	.is_enabled = clk_is_enabled_regmap,
	/* A52_PHASE11_DOWNSTREAM_CLK_SET_FLAGS_CLK_BRANCH2_OPS */
	.set_flags = clk_branch_set_flags,
};
EXPORT_SYMBOL_GPL(clk_branch2_ops);

const struct clk_ops clk_branch2_aon_ops = {
	.enable = clk_branch2_enable,
	.is_enabled = clk_is_enabled_regmap,
};
EXPORT_SYMBOL_GPL(clk_branch2_aon_ops);

const struct clk_ops clk_branch_simple_ops = {
	.enable = clk_enable_regmap,
	.disable = clk_disable_regmap,
	.is_enabled = clk_is_enabled_regmap,
};
EXPORT_SYMBOL_GPL(clk_branch_simple_ops);
