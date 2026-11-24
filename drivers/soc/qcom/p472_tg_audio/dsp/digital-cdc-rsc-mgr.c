/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Native adaptation of TG 6bf351bd techpack/audio/dsp.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <dsp/digital-cdc-rsc-mgr.h>

struct p482_vote {
	struct list_head node;
	struct clk *clk;
	unsigned int refs; /* successful outstanding client requests */
	unsigned int held; /* CCF references actually owned */
};
static DEFINE_MUTEX(hw_vote_lock);
static LIST_HEAD(hw_votes);

static struct p482_vote *find_vote(struct clk *clk)
{
	struct p482_vote *v;

	list_for_each_entry(v, &hw_votes, node)
		if (v->clk == clk)
			return v;
	return NULL;
}

static void vote_trace(struct clk *clk, const char *op, int rc,
		       unsigned int held, void *caller)
{
	const char *name = IS_ERR_OR_NULL(clk) ? "invalid" : __clk_get_name(clk);

	pr_info("P482 HW_VOTE op=%s clk=%s rc=%d held=%u caller=%pS\n",
		op, name, rc, held, caller);
#ifdef CONFIG_P480_A3C
	pr_info("P481 VOTE clk=%s op=%s count=%u caller=%pS backend=real\n",
		name, op, held, caller);
#endif
}

int digital_cdc_rsc_mgr_hw_vote_enable(struct clk *clk)
{
	struct p482_vote *v = NULL;
	int rc = -EINVAL;

	mutex_lock(&hw_vote_lock);
	if (IS_ERR_OR_NULL(clk)) {
		if (IS_ERR(clk))
			rc = PTR_ERR(clk);
		goto out;
	}
	v = find_vote(clk);
	if (!v) {
		v = kzalloc(sizeof(*v), GFP_KERNEL);
		if (!v) {
			rc = -ENOMEM;
			goto out;
		}
		v->clk = clk;
		list_add_tail(&v->node, &hw_votes);
	}
	if (v->refs != v->held) {
		rc = -EAGAIN;
		goto out;
	}
	rc = clk_prepare_enable(clk);
	if (!rc) {
		v->refs++;
		v->held++;
	}
out:
	vote_trace(clk, "enable", rc, v ? v->held : 0, (void *)_RET_IP_);
	if (v && !v->refs) {
		list_del(&v->node);
		kfree(v);
	}
	mutex_unlock(&hw_vote_lock);
	return rc;
}
EXPORT_SYMBOL(digital_cdc_rsc_mgr_hw_vote_enable);

void digital_cdc_rsc_mgr_hw_vote_disable(struct clk *clk)
{
	struct p482_vote *v;
	int rc = -EINVAL;

	mutex_lock(&hw_vote_lock);
	v = IS_ERR_OR_NULL(clk) ? NULL : find_vote(clk);
	if (v && v->refs) {
		if (v->held) {
			clk_disable_unprepare(clk);
			v->held--;
		}
		v->refs--;
		rc = 0; /* CCF unprepare is void; AFE logs transport result. */
	}
	vote_trace(clk, "disable", rc, v ? v->held : 0, (void *)_RET_IP_);
	if (v && !v->refs) {
		list_del(&v->node);
		kfree(v);
	}
	mutex_unlock(&hw_vote_lock);
}
EXPORT_SYMBOL(digital_cdc_rsc_mgr_hw_vote_disable);

/*
 * TG cycles votes after SSR, then restores them. Group matching CCF
 * cores, but release only references acquired by this manager.
 * Preserve logical counts after a failed restore for balanced release
 * or a subsequent reset; unwind partial restores.
 */
int digital_cdc_rsc_mgr_hw_vote_reset(struct clk *clk)
{
	struct p482_vote *v;
	int rc = 0;
	unsigned int held = 0;

	mutex_lock(&hw_vote_lock);
	if (IS_ERR_OR_NULL(clk)) {
		rc = -EINVAL;
		goto out;
	}
	list_for_each_entry(v, &hw_votes, node) {
		if (!clk_is_match(v->clk, clk))
			continue;
		while (v->held) {
			clk_disable_unprepare(v->clk);
			v->held--;
		}
	}
	list_for_each_entry(v, &hw_votes, node) {
		if (!clk_is_match(v->clk, clk))
			continue;
		while (v->held < v->refs) {
			rc = clk_prepare_enable(v->clk);
			if (rc)
				goto rollback;
			v->held++;
			held++;
		}
	}
	goto out;
rollback:
	list_for_each_entry(v, &hw_votes, node) {
		if (!clk_is_match(v->clk, clk))
			continue;
		while (v->held) {
			clk_disable_unprepare(v->clk);
			v->held--;
		}
	}
	held = 0;
out:
	vote_trace(clk, "reset", rc, held, (void *)_RET_IP_);
	mutex_unlock(&hw_vote_lock);
	return rc;
}
EXPORT_SYMBOL(digital_cdc_rsc_mgr_hw_vote_reset);

bool digital_cdc_rsc_mgr_hw_vote_held(struct clk *clk)
{
	struct p482_vote *v;
	bool held;

	mutex_lock(&hw_vote_lock);
	v = IS_ERR_OR_NULL(clk) ? NULL : find_vote(clk);
	held = v && v->held && v->held == v->refs;
	mutex_unlock(&hw_vote_lock);
	return held;
}
EXPORT_SYMBOL(digital_cdc_rsc_mgr_hw_vote_held);

/* Original q6_init.h hooks: native built-in lifetime is static. */
void digital_cdc_rsc_mgr_init(void) { }
void digital_cdc_rsc_mgr_exit(void) { }
