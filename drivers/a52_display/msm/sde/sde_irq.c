// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
 */

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt)	"[drm:%s:%d] " fmt, __func__, __LINE__

#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel_stat.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/atomic.h>
#include <linux/kthread.h>

#include "sde_irq.h"
#include "sde_core_irq.h"

uint32_t g_sde_irq_status;

/* A52_PHASE446M_GDSC_IRQ_AB_V1: first timeout IRQ state witness. */
static struct sde_kms *a52_p446m_kms;
static atomic_t a52_p446m_timeout_once = ATOMIC_INIT(0);

static void a52_p446m_desc_state(unsigned int irq, const char *who)
{
    struct irq_desc *desc = irq_to_desc(irq);
    pr_warn("P446M IRQ %s irq=%u depth=%d disabled=%u kstat=%u\n",
            who, irq, desc ? desc->depth : -1,
            desc ? irqd_irq_disabled(&desc->irq_data) : 0xffffffffU,
            kstat_irqs(irq));
}

void a52_p446m_snapshot_irq_timeout(void)
{
    struct sde_kms *kms;
    struct device_node *node;
    void __iomem *gic, *gdsc;
    u32 intr = 0xffffffffU, ctrl = 0xffffffffU;
    u32 gic_en = 0xffffffffU, gic_pending = 0xffffffffU;
    if (atomic_cmpxchg(&a52_p446m_timeout_once, 0, 1))
        return;
    kms = READ_ONCE(a52_p446m_kms);
    gdsc = ioremap(0x0af01004, 4);
    if (gdsc) {
        ctrl = readl_relaxed(gdsc);
        iounmap(gdsc);
    }
    node = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
    if (node) {
        gic = of_iomap(node, 0);
        of_node_put(node);
        if (gic) {
            /* GICD ISENABLER3/ISPENDR3: INTID 115 (115 / 32 = 3). */
            gic_en = readl_relaxed(gic + 0x100 + 3 * 4);
            gic_pending = readl_relaxed(gic + 0x200 + 3 * 4);
            iounmap(gic);
        }
    }
    if (kms && kms->hw_intr && (ctrl & BIT(31)))
        kms->hw_intr->ops.get_interrupt_sources(kms->hw_intr, &intr);
    pr_warn("P446M FIRST_DMA_TIMEOUT GDSCR=%08x MDSS_HW_INTR=%08x GICD115_EN=%08x GICD115_PEND=%08x mdss_irq_enabled=%d\n",
            ctrl, intr, gic_en, gic_pending,
            kms ? READ_ONCE(kms->irq_enabled) : -1);
    if (kms && kms->irq_num > 0)
        a52_p446m_desc_state(kms->irq_num, "msm_drm");
    a52_p446m_desc_state(271, "dsi_virq271");
}
EXPORT_SYMBOL_GPL(a52_p446m_snapshot_irq_timeout);

/* A52_PHASE446N_DSI_IRQ_REARM_V1: storm source sampler */
static atomic_t a52_p446n_mdss_count = ATOMIC_INIT(0);
void a52_p446n_sde_snapshot(const char *stage)
{
    struct sde_kms *kms = READ_ONCE(a52_p446m_kms);
    struct irq_desc *desc = irq_to_desc(271);
    u32 hw_src = 0xffffffffU;
    unsigned int mapping4 = 0;
    if (kms && kms->irq_controller.domain)
        mapping4 = irq_find_mapping(kms->irq_controller.domain, 4);
    if (kms && kms->hw_intr && kms->irq_enabled)
        kms->hw_intr->ops.get_interrupt_sources(kms->hw_intr, &hw_src);
    pr_warn("P446N MDSS %s mapping4=%u virq271_depth=%d disabled=%d pending=%d kstat=%u HW_INTR_STATUS=%08x last=%08x top_irq_count=%u\n",
        stage, mapping4, desc ? (int)READ_ONCE(desc->depth) : -1,
        desc ? (int)irqd_irq_disabled(&desc->irq_data) : -1,
        desc ? !!(READ_ONCE(desc->core_internal_state__do_not_mess_with_it) & 0x200) : -1,
        kstat_irqs(271), hw_src, READ_ONCE(g_sde_irq_status),
        (unsigned int)atomic_read(&a52_p446n_mdss_count));
}
EXPORT_SYMBOL_GPL(a52_p446n_sde_snapshot);


void sde_irq_update(struct msm_kms *msm_kms, bool enable)
{
	struct sde_kms *sde_kms = to_sde_kms(msm_kms);

	if (!msm_kms || !sde_kms) {
		SDE_ERROR("invalid kms arguments\n");
		return;
	}

	WRITE_ONCE(a52_p446m_kms, sde_kms);
	pr_warn("P446M IRQ update enter mdss=%d enable=%u caller=%pS\n",
		sde_kms->irq_num, enable, __builtin_return_address(0));
	a52_p446m_desc_state(sde_kms->irq_num, "before_update");
	sde_kms->irq_enabled = enable;

	if (enable)
		enable_irq(sde_kms->irq_num);
	else
		disable_irq(sde_kms->irq_num);
	a52_p446m_desc_state(sde_kms->irq_num, "after_update");
}

irqreturn_t sde_irq(struct msm_kms *kms)
{
	struct sde_kms *sde_kms = to_sde_kms(kms);
	u32 interrupts;

	sde_kms->hw_intr->ops.get_interrupt_sources(sde_kms->hw_intr,
			&interrupts);

	/* store irq status in case of irq-storm debugging */
	g_sde_irq_status = interrupts;
    {
        unsigned int n=(unsigned int)atomic_inc_return(&a52_p446n_mdss_count);
        /* Only powers of two: at most 24 messages up to 16M IRQs. */
        if (n && !(n & (n-1)))
            pr_warn("P446N MDSS_IRQ sample n=%u sources=%08x mapping4=%u\n",
                n, interrupts,
                irq_find_mapping(sde_kms->irq_controller.domain, 4));
    }

	/*
	 * Taking care of MDP interrupt
	 */
	if (interrupts & IRQ_SOURCE_MDP) {
		interrupts &= ~IRQ_SOURCE_MDP;
		sde_core_irq(sde_kms);
	}

	/*
	 * Routing all other interrupts to external drivers
	 */
	while (interrupts) {
		irq_hw_number_t hwirq = fls(interrupts) - 1;
		unsigned int mapping;
		int rc;

		mapping = irq_find_mapping(sde_kms->irq_controller.domain,
				hwirq);
		if (mapping == 0) {
			SDE_EVT32(hwirq, SDE_EVTLOG_ERROR);
			goto error;
		}

		rc = generic_handle_irq(mapping);
		if (rc < 0) {
			SDE_EVT32(hwirq, mapping, rc, SDE_EVTLOG_ERROR);
			goto error;
		}

		interrupts &= ~(1 << hwirq);
	}

	return IRQ_HANDLED;

error:
	/* bad situation, inform irq system, it may disable overall MDSS irq */
	return IRQ_NONE;
}

void sde_irq_preinstall(struct msm_kms *kms)
{
	struct sde_kms *sde_kms = to_sde_kms(kms);

	if (!sde_kms->dev || !sde_kms->dev->dev) {
		pr_err("invalid device handles\n");
		return;
	}

	sde_core_irq_preinstall(sde_kms);

	sde_kms->irq_num = platform_get_irq(
				to_platform_device(sde_kms->dev->dev),
				0);
	if (sde_kms->irq_num < 0) {
		SDE_ERROR("invalid irq number %d\n", sde_kms->irq_num);
		return;
	}

	/* disable irq until power event enables it */
	if (!sde_kms->splash_data.num_splash_displays && !sde_kms->irq_enabled)
		irq_set_status_flags(sde_kms->irq_num, IRQ_NOAUTOEN);
}

int sde_irq_postinstall(struct msm_kms *kms)
{
	struct sde_kms *sde_kms = to_sde_kms(kms);
	int rc;

	if (!kms) {
		SDE_ERROR("invalid parameters\n");
		return -EINVAL;
	}

	rc = sde_core_irq_postinstall(sde_kms);

	return rc;
}

void sde_irq_uninstall(struct msm_kms *kms)
{
	struct sde_kms *sde_kms = to_sde_kms(kms);

	if (!kms) {
		SDE_ERROR("invalid parameters\n");
		return;
	}

	sde_core_irq_uninstall(sde_kms);
	sde_core_irq_domain_fini(sde_kms);
}

/* A52_PHASE446N_DSI_IRQ_REARM_V1 variant=rearm */
