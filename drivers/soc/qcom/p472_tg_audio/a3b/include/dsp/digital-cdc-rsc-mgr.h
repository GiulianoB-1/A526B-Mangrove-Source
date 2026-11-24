/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#ifndef DIGITAL_CDC_RSC_MGR_H
#define DIGITAL_CDC_RSC_MGR_H

#ifdef CONFIG_DIGITAL_CDC_RSC_MGR

int digital_cdc_rsc_mgr_hw_vote_enable(struct clk* vote_handle);
void digital_cdc_rsc_mgr_hw_vote_disable(struct clk* vote_handle);
void digital_cdc_rsc_mgr_hw_vote_reset(struct clk* vote_handle);

#else

#ifdef CONFIG_P480_A3C
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/kernel.h>
/* Trace the pre-existing disabled-config backend without pretending its
 * local counters are real hardware votes. CCF count is software evidence.
 */
static inline void p481_stub_vote_trace(struct clk *vote_handle, const char *op,
				       void *caller)
{
	if (!IS_ERR_OR_NULL(vote_handle))
		pr_info_ratelimited("P481 VOTE clk=%s op=%s count=%u caller=%pS backend=stub ccf_count=1\n",
			__clk_get_name(vote_handle), op,
			__clk_get_enable_count(vote_handle), caller);
}
#endif

static inline int digital_cdc_rsc_mgr_hw_vote_enable(struct clk* vote_handle)
{
#ifdef CONFIG_P480_A3C
	p481_stub_vote_trace(vote_handle, "enable", (void *)_RET_IP_);
#endif
	return 0;
}

static inline void digital_cdc_rsc_mgr_hw_vote_disable(struct clk* vote_handle)
{
#ifdef CONFIG_P480_A3C
	p481_stub_vote_trace(vote_handle, "disable", (void *)_RET_IP_);
#endif
}

static inline void digital_cdc_rsc_mgr_hw_vote_reset(struct clk* vote_handle)
{
#ifdef CONFIG_P480_A3C
	p481_stub_vote_trace(vote_handle, "reset", (void *)_RET_IP_);
#endif
}

#endif /* CONFIG_DIGITAL_CDC_RSC_MGR */

#endif /* DIGITAL_CDC_RSC_MGR_H */
