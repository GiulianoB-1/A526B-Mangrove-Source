/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2020, The Linux Foundation. All rights reserved. */
#ifndef DIGITAL_CDC_RSC_MGR_H
#define DIGITAL_CDC_RSC_MGR_H
#include <linux/errno.h>
#include <linux/types.h>
struct clk;
/* P472_A3B_B1 is the local native DIGITAL_CDC_RSC_MGR equivalent. */
#if defined(CONFIG_P472_A3B_B1) || defined(CONFIG_DIGITAL_CDC_RSC_MGR)
int digital_cdc_rsc_mgr_hw_vote_enable(struct clk *vote_handle);
void digital_cdc_rsc_mgr_hw_vote_disable(struct clk *vote_handle);
int digital_cdc_rsc_mgr_hw_vote_reset(struct clk *vote_handle);
bool digital_cdc_rsc_mgr_hw_vote_held(struct clk *vote_handle);
#else
static inline int digital_cdc_rsc_mgr_hw_vote_enable(struct clk *vote_handle)
{
	return -EOPNOTSUPP;
}
static inline void digital_cdc_rsc_mgr_hw_vote_disable(struct clk *vote_handle) { }
static inline int digital_cdc_rsc_mgr_hw_vote_reset(struct clk *vote_handle)
{
	return -EOPNOTSUPP;
}
static inline bool digital_cdc_rsc_mgr_hw_vote_held(struct clk *vote_handle)
{
	return false;
}
#endif
#endif
