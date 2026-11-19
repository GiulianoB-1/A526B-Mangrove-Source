// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <asm/cacheflush.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

#include "dsi_catalog.h"
#include "dsi_ctrl_hw.h"
#include "dsi_ctrl_reg.h"
#include "dsi_hw.h"
#include "dsi_panel.h"
#include "dsi_catalog.h"
#include "sde_dbg.h"
#include <linux/ktime.h>
#include <linux/init.h>
#include <linux/module.h>


/* A52_PHASE439_DSI_AUTOPSY_V3
 *
 * Exact-F0 DSI autopsy.
 *
 * Hot path:
 *   PRE, +15 us, +35 us, +50 us and timeout snapshots are stored in normal
 *   RAM only.  No persistent recorder calls happen inside the first 50 us.
 *
 * Slow path:
 *   only after the normal ~200 ms DMA_DONE timeout do we persist the samples,
 *   sweep the link controller debug bus and deliberately panic for ramoops.
 *
 * 0x100/0x104 are Qualcomm's command-DMA scheduling controls.  The inherited
 * 4.19 driver only knows 0x100 and never initializes 0x104.  Continuous splash
 * can skip host_setup(), so both are first-class evidence for stale bootloader
 * state.
 */
#define A52_P439_PHY_PHYS          0x0ae94000ULL
#define A52_P439_PHY_SIZE          0x1000U
#define A52_P439_DMA_SCHED_CTRL    0x0100U
#define A52_P439_DMA_SCHED_CTRL2   0x0104U
#define A52_P439_MAX               10U

extern bool a52_p439_f0_active(void);
extern void a52_ackfr_record(const char *fmt, ...);

struct a52_p439_sample {
	u64 ns;
	u32 epoch, point;
	u32 status, fifo, clk_ctrl, clk_status, int_ctrl;
	u32 lane_status, lane_ctrl;
	u32 dma_ctrl, dma_offset, dma_length, sw_trigger, trig_ctrl;
	u32 sched1, sched2, disp_misc;
	u32 ack_err, timeout, phy_err, axi2ahb, dbg171;
	u32 pll_status_one, phy_pll_ctrl, phy_ctrl0, phy_rbuf, phy_clk_cfg1;
	u32 phy_lane0, phy_lane1;
};

static struct a52_p439_sample a52_p439_samples[A52_P439_MAX];
static atomic_t a52_p439_count = ATOMIC_INIT(0);
static atomic_t a52_p439_active = ATOMIC_INIT(0);
static atomic_t a52_p439_post_pending = ATOMIC_INIT(0);
static atomic_t a52_p439_epoch = ATOMIC_INIT(0);
static u32 a52_p439_current_epoch;
static void __iomem *a52_p439_phy;
static u32 a52_p439_saved_dbg;

static int __init a52_p439_map_init(void)
{
	a52_p439_phy = ioremap(A52_P439_PHY_PHYS, A52_P439_PHY_SIZE);
	return 0;
}
subsys_initcall(a52_p439_map_init);

static __always_inline u32 a52_p439_pr(u32 off)
{
	return a52_p439_phy ?
		readl_relaxed((u8 __iomem *)a52_p439_phy + off) : ~0U;
}

static void a52_p439_store(struct dsi_ctrl_hw *ctrl, u32 point)
{
	struct a52_p439_sample *s;
	unsigned int i;

	if (atomic_read(&a52_p439_active) != 1 || !ctrl || !ctrl->base)
		return;

	i = (unsigned int)atomic_inc_return(&a52_p439_count) - 1U;
	if (i >= A52_P439_MAX)
		return;

	s = &a52_p439_samples[i];
	memset(s, 0, sizeof(*s));
	s->ns = ktime_get_ns();
	s->epoch = READ_ONCE(a52_p439_current_epoch);
	s->point = point;

	s->status = DSI_R32(ctrl, DSI_STATUS);
	s->fifo = DSI_R32(ctrl, DSI_FIFO_STATUS);
	s->clk_ctrl = DSI_R32(ctrl, DSI_CLK_CTRL);
	s->clk_status = DSI_R32(ctrl, DSI_CLK_STATUS);
	s->int_ctrl = DSI_R32(ctrl, DSI_INT_CTRL);
	s->lane_status = DSI_R32(ctrl, DSI_LANE_STATUS);
	s->lane_ctrl = DSI_R32(ctrl, DSI_LANE_CTRL);

	s->dma_ctrl = DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL);
	s->dma_offset = DSI_R32(ctrl, DSI_DMA_CMD_OFFSET);
	s->dma_length = DSI_R32(ctrl, DSI_DMA_CMD_LENGTH);
	s->sw_trigger = DSI_R32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER);
	s->trig_ctrl = DSI_R32(ctrl, DSI_TRIG_CTRL);

	s->sched1 = DSI_R32(ctrl, A52_P439_DMA_SCHED_CTRL);
	s->sched2 = DSI_R32(ctrl, A52_P439_DMA_SCHED_CTRL2);
	s->disp_misc = ctrl->disp_cc_base ? DSI_DISP_CC_R32(ctrl, 0x0) : ~0U;

	s->ack_err = DSI_R32(ctrl, DSI_ACK_ERR_STATUS);
	s->timeout = DSI_R32(ctrl, DSI_TIMEOUT_STATUS);
	s->phy_err = DSI_R32(ctrl, DSI_DLN0_PHY_ERR);
	s->axi2ahb = DSI_R32(ctrl, DSI_AXI2AHB_CTRL);
	s->dbg171 = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);

	s->phy_clk_cfg1 = a52_p439_pr(0x414);
	s->phy_rbuf = a52_p439_pr(0x41c);
	s->phy_ctrl0 = a52_p439_pr(0x424);
	s->phy_pll_ctrl = a52_p439_pr(0x438);
	s->phy_lane0 = a52_p439_pr(0x4f4);
	s->phy_lane1 = a52_p439_pr(0x4f8);
	s->pll_status_one = a52_p439_pr(0xba0);
}

static __always_inline void a52_p439_wait_until(u64 start, u64 delta)
{
	while ((ktime_get_ns() - start) < delta)
		cpu_relax();
}

static void a52_p439_begin(struct dsi_ctrl_hw *ctrl)
{
	if (!a52_p439_f0_active() || !ctrl || !ctrl->base)
		return;
	if (atomic_cmpxchg(&a52_p439_active, 0, 1) != 0)
		return;

	WRITE_ONCE(a52_p439_current_epoch,
		   (u32)atomic_inc_return(&a52_p439_epoch));

	/*
	 * Selector 0x0171 is our one precisely timed internal-state probe.
	 * A broad selector sweep is intentionally deferred until timeout.
	 */
	a52_p439_saved_dbg = DSI_R32(ctrl, DSI_DEBUG_BUS_CTL);
	DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, 0x0171);
	wmb();

	atomic_set(&a52_p439_post_pending, 1);
	a52_p439_store(ctrl, 0U);
}

static void a52_p439_post_trigger(struct dsi_ctrl_hw *ctrl)
{
	u64 start;

	if (atomic_cmpxchg(&a52_p439_post_pending, 1, 0) != 1 ||
	    !ctrl || !ctrl->base)
		return;

	start = ktime_get_ns();
	a52_p439_wait_until(start, 15000ULL);
	a52_p439_store(ctrl, 15U);
	a52_p439_wait_until(start, 35000ULL);
	a52_p439_store(ctrl, 35U);
	a52_p439_wait_until(start, 50000ULL);
	a52_p439_store(ctrl, 50U);

	DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, a52_p439_saved_dbg);
	wmb();
}

void a52_p439_flush_samples(struct dsi_ctrl_hw *ctrl);

void a52_p439_allow_next(void)
{
	/*
	 * Phase440 persists the successful early epoch before re-arming. This is
	 * what makes PASS->PASS observable even if no timeout ever flushes P439.
	 */
	if (atomic_read(&a52_p439_active) == 1)
		a52_p439_flush_samples(NULL);
	a52_ackfr_record("P440 EARLY rearm count=%d epoch=%d",
			 atomic_read(&a52_p439_count),
			 atomic_read(&a52_p439_epoch));
	atomic_set(&a52_p439_post_pending, 0);
	atomic_set(&a52_p439_active, 0);
}
EXPORT_SYMBOL_GPL(a52_p439_allow_next);

void a52_p439_timeout_snapshot(struct dsi_ctrl_hw *ctrl)
{
	if (atomic_read(&a52_p439_active) != 1 || !a52_p439_f0_active())
		return;
	a52_p439_store(ctrl, 200U);
}

void a52_p439_flush_samples(struct dsi_ctrl_hw *ctrl)
{
	unsigned int i;
	unsigned int n = (unsigned int)atomic_read(&a52_p439_count);
	u64 base = 0;

	if (n > A52_P439_MAX)
		n = A52_P439_MAX;
	if (n)
		base = a52_p439_samples[0].ns;

	for (i = 0; i < n; i++) {
		const struct a52_p439_sample *s = &a52_p439_samples[i];

		a52_ackfr_record(
			"P439 S i=%u e=%u p=%u us=%llu st=%x fs=%x cc=%x ck=%x in=%x ln=%x db=%x",
			i, s->epoch, s->point,
			(unsigned long long)((s->ns - base) / 1000ULL),
			s->status, s->fifo, s->clk_ctrl, s->clk_status,
			s->int_ctrl, s->lane_status, s->dbg171);

		a52_ackfr_record(
			"P439 D i=%u e=%u dc=%x o=%x l=%x sw=%x tg=%x ae=%x to=%x pe=%x ax=%x",
			i, s->epoch, s->dma_ctrl, s->dma_offset,
			s->dma_length, s->sw_trigger, s->trig_ctrl,
			s->ack_err, s->timeout, s->phy_err, s->axi2ahb);

		a52_ackfr_record(
			"P439 Q i=%u e=%u s1=%x s2=%x dm=%x lc=%x",
			i, s->epoch, s->sched1, s->sched2,
			s->disp_misc, s->lane_ctrl);

		a52_ackfr_record(
			"P439 P i=%u e=%u ps=%x pc=%x c0=%x rb=%x cf=%x l0=%x l1=%x",
			i, s->epoch, s->pll_status_one, s->phy_pll_ctrl,
			s->phy_ctrl0, s->phy_rbuf, s->phy_clk_cfg1,
			s->phy_lane0, s->phy_lane1);
	}

	if (ctrl && ctrl->base)
		a52_ackfr_record(
			"P439 T st=%x in=%x cc=%x ck=%x s1=%x s2=%x tg=%x n=%u",
			DSI_R32(ctrl, DSI_STATUS),
			DSI_R32(ctrl, DSI_INT_CTRL),
			DSI_R32(ctrl, DSI_CLK_CTRL),
			DSI_R32(ctrl, DSI_CLK_STATUS),
			DSI_R32(ctrl, A52_P439_DMA_SCHED_CTRL),
			DSI_R32(ctrl, A52_P439_DMA_SCHED_CTRL2),
			DSI_R32(ctrl, DSI_TRIG_CTRL), n);
}

void a52_p439_debug_sweep(struct dsi_ctrl_hw *ctrl)
{
	u32 saved, block, test, ctl, value;

	if (!ctrl || !ctrl->base)
		return;

	saved = DSI_R32(ctrl, DSI_DEBUG_BUS_CTL);

	/*
	 * This is deliberately post-timeout.  It cannot perturb the 15/35/50 us
	 * timing experiment because the command has already failed.
	 */
	for (block = 0; block < 4; block++) {
		for (test = 0; test < 64; test++) {
			ctl = ((block & 0x3U) << 12) |
			      ((test & 0x3fU) << 4) | BIT(0);
			DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, ctl);
			wmb();
			value = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);
			a52_ackfr_record("P439 B b=%u t=%u c=%x v=%x",
					 block, test, ctl, value);
		}
	}

	DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, saved);
	wmb();
}

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>

static int a52_p439_dbg_show(struct seq_file *m, void *unused)
{
	unsigned int i;
	unsigned int n = (unsigned int)atomic_read(&a52_p439_count);

	if (n > A52_P439_MAX)
		n = A52_P439_MAX;

	seq_printf(m, "%s count=%u active=%d epoch=%d\n",
		   "A52_PHASE439_DSI_AUTOPSY_V3", n,
		   atomic_read(&a52_p439_active),
		   atomic_read(&a52_p439_epoch));

	for (i = 0; i < n; i++) {
		const struct a52_p439_sample *s = &a52_p439_samples[i];

		seq_printf(m,
			   "i=%u e=%u p=%u ns=%llu st=%08x in=%08x ck=%08x cc=%08x db=%08x s1=%08x s2=%08x dm=%08x ps=%08x pc=%08x\n",
			   i, s->epoch, s->point,
			   (unsigned long long)s->ns,
			   s->status, s->int_ctrl,
			   s->clk_status, s->clk_ctrl, s->dbg171,
			   s->sched1, s->sched2, s->disp_misc,
			   s->pll_status_one, s->phy_pll_ctrl);
	}
	return 0;
}

static int a52_p439_dbg_open(struct inode *inode, struct file *file)
{
	return single_open(file, a52_p439_dbg_show, NULL);
}

static const struct file_operations a52_p439_dbg_fops = {
	.owner = THIS_MODULE,
	.open = a52_p439_dbg_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init a52_p439_dbg_init(void)
{
	debugfs_create_file("a52_phase439_dsi", 0444, NULL, NULL,
			    &a52_p439_dbg_fops);
	return 0;
}
late_initcall(a52_p439_dbg_init);
#endif

/* A52_PHASE421_FETCH_MEMORY_SURVIVAL_V1 */
extern bool a52_p421_target_active(void);
extern void a52_p421_survival_record(u8 stage, u8 valid, u32 flags,
				     int rc, u32 irq, int ret);
extern void a52_ackfr_record(const char *fmt, ...);

/* A52_PHASE424_PAIRED_HW_SNAPSHOT_V1
 *
 * Read-only paired Golden/GKI snapshot at the exact first F0 command-DMA
 * pre-trigger point.  No register write, reset, vote, clock, regulator or
 * RSC state change is performed here.
 *
 * Physical mappings are deliberately identical to Phase424G Golden:
 *   DISP_CC 0x0af00000, GCC 0x00100000, VBIF 0x0aeb0000,
 *   RSC drv 0x0af20000, RSC wrapper 0x0af30000.
 */
#define A52_P424_DISPCC_PHYS  0x0af00000ULL
#define A52_P424_GCC_PHYS     0x00100000ULL
#define A52_P424_VBIF_PHYS    0x0aeb0000ULL
#define A52_P424_RSC_DRV_PHYS 0x0af20000ULL
#define A52_P424_RSC_WRP_PHYS 0x0af30000ULL
#define A52_P434_PHYPLL_PHYS  0x0ae94000ULL
#define A52_P434_INTF1_PHYS   0x0ae6b000ULL

struct a52_p424_snapshot {
	u64 ns;
	u32 dsi[8];
	u32 clk0[8];
	u32 clk1[8];
	u32 pwr[4];
	u32 vbif0[8];
	u32 vbif1[4];
	u32 rsc0[8];
	u32 rsc1[6];
};

static struct a52_p424_snapshot a52_p424_snap;
static atomic_t a52_p424_taken = ATOMIC_INIT(0);
static void __iomem *a52_p424_dispcc;
static void __iomem *a52_p424_gcc;
static void __iomem *a52_p424_vbif;
static void __iomem *a52_p424_rsc_drv;
static void __iomem *a52_p424_rsc_wrp;
static void __iomem *a52_p434_phypll;
static void __iomem *a52_p434_intf1;

#define A52_P427_HF0_GCC_PHYS 0x001b7000ULL
static void __iomem *a52_p427_hf0_gcc;

static int __init a52_p424_map_init(void)
{
	a52_p424_dispcc = ioremap(A52_P424_DISPCC_PHYS, 0x3000);
	a52_p424_gcc = ioremap(A52_P424_GCC_PHYS, 0x18000);
	a52_p424_vbif = ioremap(A52_P424_VBIF_PHYS, 0x1000);
	a52_p424_rsc_drv = ioremap(A52_P424_RSC_DRV_PHYS, 0x2400);
	a52_p424_rsc_wrp = ioremap(A52_P424_RSC_WRP_PHYS, 0x100);
	a52_p434_phypll = ioremap(A52_P434_PHYPLL_PHYS, 0x1000);
	a52_p434_intf1 = ioremap(A52_P434_INTF1_PHYS, 0x1000);
	a52_p427_hf0_gcc = ioremap(A52_P427_HF0_GCC_PHYS, 0x1000);
	return 0;
}
subsys_initcall(a52_p424_map_init);

static inline u32 a52_p424_r(void __iomem *b, u32 o)
{
	return b ? readl_relaxed((u8 __iomem *)b + o) : ~0U;
}

void a52_p424_snapshot(struct dsi_ctrl_hw *ctrl)
{
	struct a52_p424_snapshot *s=&a52_p424_snap;

	if (!a52_p421_target_active() || !ctrl || !ctrl->base)
		return;
	if (atomic_cmpxchg(&a52_p424_taken,0,1) != 0)
		return;

	memset(s,0,sizeof(*s));
	s->ns = ktime_get_boottime_ns();

	/* Small DSI alignment anchor. Full DSI/PHY prestate was already covered
	 * by the Phase314/315G line of experiments.
	 */
	s->dsi[0]=DSI_R32(ctrl,DSI_STATUS);
	s->dsi[1]=DSI_R32(ctrl,DSI_LANE_STATUS);
	s->dsi[2]=DSI_R32(ctrl,DSI_CLK_STATUS);
	s->dsi[3]=DSI_R32(ctrl,DSI_INT_CTRL);
	s->dsi[4]=DSI_R32(ctrl,DSI_COMMAND_MODE_DMA_CTRL);
	s->dsi[5]=DSI_R32(ctrl,DSI_DMA_CMD_OFFSET);
	s->dsi[6]=DSI_R32(ctrl,DSI_DMA_CMD_LENGTH);
	s->dsi[7]=DSI_R32(ctrl,DSI_AXI2AHB_CTRL);

	/* DISP_CC branch state + GCC DISP fabric clocks. */
	s->clk0[0]=a52_p424_r(a52_p424_dispcc,0x100c); /* pclk0 */
	s->clk0[1]=a52_p424_r(a52_p424_dispcc,0x1010); /* mdp */
	s->clk0[2]=a52_p424_r(a52_p424_dispcc,0x102c); /* byte0 */
	s->clk0[3]=a52_p424_r(a52_p424_dispcc,0x1030); /* byte0 intf */
	s->clk0[4]=a52_p424_r(a52_p424_dispcc,0x1034); /* esc0 */
	s->clk0[5]=a52_p424_r(a52_p424_dispcc,0x104c); /* mdss ahb */
	s->clk0[6]=a52_p424_r(a52_p424_gcc,0x1700c);   /* gcc disp ahb */
	s->clk0[7]=a52_p424_r(a52_p424_gcc,0x1701c);   /* gcc disp axi */

	/* Active RCG command/config registers. */
	s->clk1[0]=a52_p424_r(a52_p424_dispcc,0x1064);
	s->clk1[1]=a52_p424_r(a52_p424_dispcc,0x1068);
	s->clk1[2]=a52_p424_r(a52_p424_dispcc,0x107c);
	s->clk1[3]=a52_p424_r(a52_p424_dispcc,0x1080);
	s->clk1[4]=a52_p424_r(a52_p424_dispcc,0x10c4);
	s->clk1[5]=a52_p424_r(a52_p424_dispcc,0x10c8);
	s->clk1[6]=a52_p424_r(a52_p424_dispcc,0x10e0);
	s->clk1[7]=a52_p424_r(a52_p424_dispcc,0x10e4);

	/* MDSS GDSC and RSCC branch clocks. */
	s->pwr[0]=a52_p424_r(a52_p424_dispcc,0x1004);
	s->pwr[1]=a52_p424_r(a52_p424_dispcc,0x2004);
	s->pwr[2]=a52_p424_r(a52_p424_dispcc,0x2008);
	s->pwr[3]=a52_p424_r(a52_p424_dispcc,0x200c);

	/* VBIF read-only state. This is retained as an exploratory fabric
	 * reference; DSI command DMA is not treated as a proven VBIF client.
	 */
	s->vbif0[0]=a52_p424_r(a52_p424_vbif,0x0000);
	s->vbif0[1]=a52_p424_r(a52_p424_vbif,0x0008);
	s->vbif0[2]=a52_p424_r(a52_p424_vbif,0x000c);
	s->vbif0[3]=a52_p424_r(a52_p424_vbif,0x0190);
	s->vbif0[4]=a52_p424_r(a52_p424_vbif,0x0194);
	s->vbif0[5]=a52_p424_r(a52_p424_vbif,0x0200);
	s->vbif0[6]=a52_p424_r(a52_p424_vbif,0x0204);
	s->vbif0[7]=a52_p424_r(a52_p424_vbif,0x00b0);
	s->vbif1[0]=a52_p424_r(a52_p424_vbif,0x0020);
	s->vbif1[1]=a52_p424_r(a52_p424_vbif,0x0024);
	s->vbif1[2]=a52_p424_r(a52_p424_vbif,0x0550);
	s->vbif1[3]=a52_p424_r(a52_p424_vbif,0x0590);

	/* Display RSC hardware state. */
	s->rsc0[0]=a52_p424_r(a52_p424_rsc_drv,0x0404); /* seq busy */
	s->rsc0[1]=a52_p424_r(a52_p424_rsc_drv,0x0408); /* seq PC */
	s->rsc0[2]=a52_p424_r(a52_p424_rsc_drv,0x00d0); /* error irq */
	s->rsc0[3]=a52_p424_r(a52_p424_rsc_drv,0x0c14); /* solver override */
	s->rsc0[4]=a52_p424_r(a52_p424_rsc_drv,0x0c20); /* modes enabled */
	s->rsc0[5]=a52_p424_r(a52_p424_rsc_drv,0x0c24); /* solver status0 */
	s->rsc0[6]=a52_p424_r(a52_p424_rsc_drv,0x0c28); /* solver status1 */
	s->rsc0[7]=a52_p424_r(a52_p424_rsc_drv,0x0c2c); /* solver status2 */
	s->rsc1[0]=a52_p424_r(a52_p424_rsc_drv,0x1c00); /* AMC IRQ */
	s->rsc1[1]=a52_p424_r(a52_p424_rsc_drv,0x1c14); /* TCS control */
	s->rsc1[2]=a52_p424_r(a52_p424_rsc_wrp,0x0000); /* wrapper ctrl */
	s->rsc1[3]=a52_p424_r(a52_p424_rsc_wrp,0x0004); /* override ctrl */
	s->rsc1[4]=a52_p424_r(a52_p424_rsc_wrp,0x0024); /* power ctrl */
	s->rsc1[5]=a52_p424_r(a52_p424_rsc_wrp,0x0048); /* BW indication */
}
EXPORT_SYMBOL_GPL(a52_p424_snapshot);

void a52_p424_dump_snapshot(void)
{
	const struct a52_p424_snapshot *s=&a52_p424_snap;

	if (!atomic_read(&a52_p424_taken))
		return;

	a52_ackfr_record("P424 T ns=%llu",(unsigned long long)s->ns);
	a52_ackfr_record("P424 D %x %x %x %x %x %x %x %x",
		s->dsi[0],s->dsi[1],s->dsi[2],s->dsi[3],
		s->dsi[4],s->dsi[5],s->dsi[6],s->dsi[7]);
	a52_ackfr_record("P424 C0 %x %x %x %x %x %x %x %x",
		s->clk0[0],s->clk0[1],s->clk0[2],s->clk0[3],
		s->clk0[4],s->clk0[5],s->clk0[6],s->clk0[7]);
	a52_ackfr_record("P424 C1 %x %x %x %x %x %x %x %x",
		s->clk1[0],s->clk1[1],s->clk1[2],s->clk1[3],
		s->clk1[4],s->clk1[5],s->clk1[6],s->clk1[7]);
	a52_ackfr_record("P424 P %x %x %x %x",
		s->pwr[0],s->pwr[1],s->pwr[2],s->pwr[3]);
	a52_ackfr_record("P424 V0 %x %x %x %x %x %x %x %x",
		s->vbif0[0],s->vbif0[1],s->vbif0[2],s->vbif0[3],
		s->vbif0[4],s->vbif0[5],s->vbif0[6],s->vbif0[7]);
	a52_ackfr_record("P424 V1 %x %x %x %x",
		s->vbif1[0],s->vbif1[1],s->vbif1[2],s->vbif1[3]);
	a52_ackfr_record("P424 R0 %x %x %x %x %x %x %x %x",
		s->rsc0[0],s->rsc0[1],s->rsc0[2],s->rsc0[3],
		s->rsc0[4],s->rsc0[5],s->rsc0[6],s->rsc0[7]);
	a52_ackfr_record("P424 R1 %x %x %x %x %x %x",
		s->rsc1[0],s->rsc1[1],s->rsc1[2],
		s->rsc1[3],s->rsc1[4],s->rsc1[5]);
}
EXPORT_SYMBOL_GPL(a52_p424_dump_snapshot);


/* A52_PHASE427_CB2_HANDOFF_ROAD_PROBE_V1
 * Road-to-memory snapshot.  The pre-trigger call executes after all DSI DMA
 * programming and before SW_TRIGGER.  It performs only normal-RAM loads and
 * MMIO reads.  No clock, regulator, RPMh, bus-vote, SMMU or RSC write occurs.
 */
#define A52_P427_TCS_BASE     0x1c00U
#define A52_P427_TCS_STRIDE   0x02a0U
#define A52_P427_CMD_STRIDE   0x0014U
#define A52_P427_MAX_CMDS     16U
#define A52_P427_BUS_SLOTS    4U
#define A52_P427_BCM_SLOTS    7U
#define A52_P427_SNAPS        2U

struct a52_p427_tcs_cmd {
	u32 addr;
	u32 data;
	u32 status;
};
struct a52_p427_tcs {
	u32 control;
	u32 status;
	u32 enable;
	u32 wait;
	struct a52_p427_tcs_cmd cmd[A52_P427_MAX_CMDS];
};
struct a52_p427_bus_sample {
	u32 valid;
	u32 handle;
	s32 curr;
	u32 num_paths;
	u64 ab;
	u64 ib;
};
struct a52_p427_bcm_sample {
	u32 valid;
	u32 addr;
	u64 vec_a;
	u64 vec_b;
};
struct a52_p427_road_snapshot {
	u64 ns;
	u32 valid;
	u32 child_cfg;
	u32 num_tcs;
	u32 ncpt;
	s32 rsc_state;
	u32 gcc40;
	u32 gcc44;
	u32 tbu_valid;
	u32 tbu_status;
	u32 sacr_seen;
	u32 sacr_before;
	u32 sacr_after;
	u32 sacr_current;
	struct a52_p427_tcs tcs[2];
	struct a52_p427_bus_sample bus[A52_P427_BUS_SLOTS];
	struct a52_p427_bcm_sample bcm[A52_P427_BCM_SLOTS];
};

static struct a52_p427_road_snapshot a52_p427_road[A52_P427_SNAPS];
static atomic_t a52_p427_taken[A52_P427_SNAPS] = {
	ATOMIC_INIT(0), ATOMIC_INIT(0)
};
extern void a52_p336_qsmmuv500_snapshot(u32 *seen, u32 *sacr_before,
		u32 *sacr_after, u32 *sacr_current, u32 *tbu_valid, u32 *tbu_status);
extern void a52_p427_bus_get(u32 slot, u32 *valid, u32 *handle, s32 *curr,
		u32 *num_paths, u64 *ab, u64 *ib);
extern void a52_p427_bcm_get(u32 slot, u32 *valid, u32 *addr,
		u64 *vec_a, u64 *vec_b);
extern int get_sde_rsc_current_state(int rsc_index);

static void a52_p427_read_tcs(struct a52_p427_road_snapshot *s, u32 t)
{
	void __iomem *b = a52_p424_rsc_drv;
	u32 base = A52_P427_TCS_BASE + t * A52_P427_TCS_STRIDE;
	u32 n;

	if (!b || t >= 2)
		return;
	s->tcs[t].wait = a52_p424_r(b, base + 0x10);
	s->tcs[t].control = a52_p424_r(b, base + 0x14);
	s->tcs[t].status = a52_p424_r(b, base + 0x18);
	s->tcs[t].enable = a52_p424_r(b, base + 0x1c);
	for (n = 0; n < min_t(u32, s->ncpt, A52_P427_MAX_CMDS); n++) {
		u32 c = base + 0x30 + n * A52_P427_CMD_STRIDE;
		s->tcs[t].cmd[n].addr = a52_p424_r(b, c + 0x04);
		s->tcs[t].cmd[n].data = a52_p424_r(b, c + 0x08);
		s->tcs[t].cmd[n].status = a52_p424_r(b, c + 0x0c);
	}
}

void a52_p427_road_snapshot(struct dsi_ctrl_hw *ctrl, unsigned int point)
{
	struct a52_p427_road_snapshot *s;
	u32 i;

	if (point >= A52_P427_SNAPS || !ctrl || !ctrl->base)
		return;
	if (atomic_cmpxchg(&a52_p427_taken[point], 0, 1) != 0)
		return;

	s = &a52_p427_road[point];
	memset(s, 0, sizeof(*s));
	s->ns = ktime_get_boottime_ns();
	s->valid = 1;
	s->child_cfg = a52_p424_r(a52_p424_rsc_drv, 0x0c);
	s->num_tcs = s->child_cfg & 0x3f;
	s->ncpt = (s->child_cfg >> 27) & 0x1f;
	s->rsc_state = get_sde_rsc_current_state(0);
	s->gcc40 = a52_p424_r(a52_p427_hf0_gcc, 0x40);
	s->gcc44 = a52_p424_r(a52_p427_hf0_gcc, 0x44);
	a52_p336_qsmmuv500_snapshot(&s->sacr_seen, &s->sacr_before,
		&s->sacr_after, &s->sacr_current, &s->tbu_valid, &s->tbu_status);

	for (i = 0; i < min_t(u32, s->num_tcs, 2U); i++)
		a52_p427_read_tcs(s, i);
	for (i = 0; i < A52_P427_BUS_SLOTS; i++)
		a52_p427_bus_get(i, &s->bus[i].valid, &s->bus[i].handle,
			&s->bus[i].curr, &s->bus[i].num_paths,
			&s->bus[i].ab, &s->bus[i].ib);
	for (i = 0; i < A52_P427_BCM_SLOTS; i++)
		a52_p427_bcm_get(i, &s->bcm[i].valid, &s->bcm[i].addr,
			&s->bcm[i].vec_a, &s->bcm[i].vec_b);
	smp_wmb();
}
EXPORT_SYMBOL_GPL(a52_p427_road_snapshot);

void a52_p427_road_dump(void)
{
	u32 p, t, n, i;

	for (p = 0; p < A52_P427_SNAPS; p++) {
		const struct a52_p427_road_snapshot *s = &a52_p427_road[p];
		if (!READ_ONCE(s->valid))
			continue;
		a52_ackfr_record("P427 R p=%u cf=%x nt=%u nc=%u rs=%d",
			p, s->child_cfg, s->num_tcs, s->ncpt, s->rsc_state);
		a52_ackfr_record("P427 G p=%u g0=%x g1=%x tv=%u ts=%x",
			p, s->gcc40, s->gcc44, s->tbu_valid, s->tbu_status);
		a52_ackfr_record("P427 A p=%u v=%u b=%x a=%x c=%x",
			p, s->sacr_seen, s->sacr_before, s->sacr_after,
			s->sacr_current);
		for (t = 0; t < min_t(u32, s->num_tcs, 2U); t++) {
			a52_ackfr_record("P427 Q p=%u t=%u c=%x e=%x w=%x s=%x",
				p, t, s->tcs[t].control, s->tcs[t].enable,
				s->tcs[t].wait, s->tcs[t].status);
			for (n = 0; n < min_t(u32, s->ncpt, A52_P427_MAX_CMDS); n++)
				a52_ackfr_record("P427 T p=%u t=%u n=%u a=%x d=%x s=%x",
					p, t, n, s->tcs[t].cmd[n].addr,
					s->tcs[t].cmd[n].data,
					s->tcs[t].cmd[n].status);
		}
		for (i = 0; i < A52_P427_BUS_SLOTS; i++) {
			a52_ackfr_record("P427 U0 p=%u s=%u v=%u h=%u c=%d n=%u",
				p, i, s->bus[i].valid, s->bus[i].handle,
				s->bus[i].curr, s->bus[i].num_paths);
			a52_ackfr_record("P427 U1 p=%u s=%u a=%llx i=%llx",
				p, i, (unsigned long long)s->bus[i].ab,
				(unsigned long long)s->bus[i].ib);
		}
		for (i = 0; i < A52_P427_BCM_SLOTS; i++) {
			a52_ackfr_record("P427 B0 p=%u s=%u v=%u x=%x",
				p, i, s->bcm[i].valid, s->bcm[i].addr);
			a52_ackfr_record("P427 B1 p=%u s=%u a=%llx b=%llx",
				p, i, (unsigned long long)s->bcm[i].vec_a,
				(unsigned long long)s->bcm[i].vec_b);
		}
	}
}
EXPORT_SYMBOL_GPL(a52_p427_road_dump);


/* A52_PHASE430_SF_DMA_FORENSICS_V1
 * Exact-F0 DMA forensic image.  Timing samples stay in normal RAM until the
 * completion outcome is known; timeout then persists three CRC32C copies into
 * the dormant upper pmsg sideband before the deliberate panic.
 */
#define A52_P430_DMA_PHYS          0xB1BF8000ULL
#define A52_P430_DMA_BYTES         0x1800U
#define A52_P430_DMA_COPY_BYTES    0x0800U
#define A52_P430_DMA_MAGIC         0x303334414d445349ULL
#define A52_P430_DMA_COMMIT        0x430d0de5U
#define A52_P430_DMA_VERSION       1U
#define A52_P430_DMA_SAMPLES       11U
#define A52_P430_TCS_CMDS          16U

struct a52_p430_dma_sample {
	u64 ns;
	u32 point;
	u32 status;
	u32 fifo;
	u32 clk_ctrl;
	u32 clk_status;
	u32 int_ctrl;
	u32 lane_status;
	u32 dma_ctrl;
	u32 dma_offset;
	u32 dma_length;
	u32 sw_trigger;
	u32 trig_ctrl;
	u32 ack_err;
	u32 timeout_status;
	u32 phy_err;
	u32 axi2ahb;
	u32 err_mask0;
	u32 ctrl;
	u32 lane_ctrl;
	u32 pll_common_status_one;
	u32 phy_pll_cntrl;
	u32 phy_ctrl0;
	u32 phy_rbuf_ctrl;
	u32 phy_clk_cfg1;
	u32 intf1_frame_count;
	u32 intf1_te_count;
} __packed;

struct a52_p430_tcs_cmd { u32 addr, data, status; } __packed;
struct a52_p430_tcs {
	u32 control, status, enable, wait;
	struct a52_p430_tcs_cmd cmd[A52_P430_TCS_CMDS];
} __packed;

struct a52_p430_smmu_diag {
	u32 valid, rpm_ret, sid, sme, cb, irq;
	u32 smr, s2cr, cbar, cba2r, cbfrsynra;
	u32 sctlr, actlr, tcr, tcr2, mair0, mair1, contextidr;
	u32 fsr, fsynr0, gfsr, gfsynr0, gfsynr1, gfsynr2;
	u32 tlbstatus, gtlbstatus, atos_status, atos_timeout;
	u64 ttbr0, ttbr1, far, sw_phys, hw_phys, par;
} __packed;

struct a52_p430_dma_image {
	u64 magic;
	u64 capture_ns;
	u64 iova;
	u32 sample_count;
	u32 wait_ret;
	u32 irq_trig;
	u32 bad_iova;
	u32 child_cfg;
	u32 num_tcs;
	u32 ncpt;
	u32 valid;
	struct a52_p430_dma_sample sample[A52_P430_DMA_SAMPLES];
	struct a52_p430_tcs tcs[2];
	u32 clk[20];
	u32 pwr[4];
	struct a52_p430_smmu_diag smmu;
	u32 crc32c;
	u32 commit;
	u32 version;
} __packed;

static struct a52_p430_dma_image a52_p430_dma;
static void *a52_p430_dma_sideband;
static struct dsi_ctrl_hw *a52_p430_dma_ctrl;
static atomic_t a52_p430_dma_armed = ATOMIC_INIT(0);
extern int a52_p430_smmu_forensic(u64 iova, void *out, size_t len);
extern bool a52_p430_bad_iova_active(void);
extern void a52_p434_dump_rails(const char *tag);

static u32 a52_p430_crc32c(const void *buffer, size_t len)
{
	const u8 *bytes = buffer;
	u32 crc = ~0U;
	size_t index;
	unsigned int bit;
	for (index = 0; index < len; index++) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1U) ? 0x82f63b78U : 0U);
	}
	return ~crc;
}

static void a52_p430_dma_sample(struct dsi_ctrl_hw *ctrl, u32 point)
{
	struct a52_p430_dma_sample *s;
	u32 i = READ_ONCE(a52_p430_dma.sample_count);
	if (!ctrl || !ctrl->base || i >= A52_P430_DMA_SAMPLES)
		return;
	s = &a52_p430_dma.sample[i];
	memset(s, 0, sizeof(*s));
	s->ns = ktime_get_ns();
	s->point = point;
	s->status = DSI_R32(ctrl, DSI_STATUS);
	s->fifo = DSI_R32(ctrl, DSI_FIFO_STATUS);
	s->clk_ctrl = DSI_R32(ctrl, DSI_CLK_CTRL);
	s->clk_status = DSI_R32(ctrl, DSI_CLK_STATUS);
	s->int_ctrl = DSI_R32(ctrl, DSI_INT_CTRL);
	s->lane_status = DSI_R32(ctrl, DSI_LANE_STATUS);
	s->dma_ctrl = DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL);
	s->dma_offset = DSI_R32(ctrl, DSI_DMA_CMD_OFFSET);
	s->dma_length = DSI_R32(ctrl, DSI_DMA_CMD_LENGTH);
	s->sw_trigger = DSI_R32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER);
	s->trig_ctrl = DSI_R32(ctrl, DSI_TRIG_CTRL);
	s->ack_err = DSI_R32(ctrl, DSI_ACK_ERR_STATUS);
	s->timeout_status = DSI_R32(ctrl, DSI_TIMEOUT_STATUS);
	s->phy_err = DSI_R32(ctrl, DSI_DLN0_PHY_ERR);
	s->axi2ahb = DSI_R32(ctrl, DSI_AXI2AHB_CTRL);
	s->err_mask0 = DSI_R32(ctrl, DSI_ERR_INT_MASK0);
	s->ctrl = DSI_R32(ctrl, DSI_CTRL);
	s->lane_ctrl = DSI_R32(ctrl, DSI_LANE_CTRL);
	s->pll_common_status_one = a52_p424_r(a52_p434_phypll, 0xBA0);
	s->phy_pll_cntrl = a52_p424_r(a52_p434_phypll, 0x438);
	s->phy_ctrl0 = a52_p424_r(a52_p434_phypll, 0x424);
	s->phy_rbuf_ctrl = a52_p424_r(a52_p434_phypll, 0x41C);
	s->phy_clk_cfg1 = a52_p424_r(a52_p434_phypll, 0x414);
	s->intf1_frame_count = a52_p424_r(a52_p434_intf1, 0x8AC);
	s->intf1_te_count = a52_p424_r(a52_p434_intf1, 0xA98);
	WRITE_ONCE(a52_p430_dma.sample_count, i + 1U);
}


void a52_p430_dma_arm(struct dsi_ctrl_hw *ctrl)
{
	if (!a52_p421_target_active() || !ctrl || !ctrl->base)
		return;
	if (atomic_cmpxchg(&a52_p430_dma_armed, 0, 1) != 0)
		return;
	memset(&a52_p430_dma, 0, sizeof(a52_p430_dma));
	a52_p430_dma.magic = A52_P430_DMA_MAGIC;
	a52_p430_dma.valid = 1U;
	a52_p430_dma.bad_iova = a52_p430_bad_iova_active() ? 1U : 0U;
	WRITE_ONCE(a52_p430_dma_ctrl, ctrl);
	a52_p434_dump_rails("pre");
	a52_p430_dma_sample(ctrl, 0U);
}
EXPORT_SYMBOL_GPL(a52_p430_dma_arm);

void a52_p430_dma_triggered(struct dsi_ctrl_hw *ctrl)
{
	if (!atomic_read(&a52_p430_dma_armed) ||
	    ctrl != READ_ONCE(a52_p430_dma_ctrl))
		return;
	a52_p430_dma_sample(ctrl, 1U);
	udelay(1);
	a52_p430_dma_sample(ctrl, 2U);
	udelay(4);
	a52_p430_dma_sample(ctrl, 3U);
	udelay(15);
	a52_p430_dma_sample(ctrl, 4U);
	udelay(80);
	a52_p430_dma_sample(ctrl, 5U);
}


void a52_p430_dma_event(struct dsi_ctrl_hw *ctrl, u32 point)
{
	if (!atomic_read(&a52_p430_dma_armed) ||
	    ctrl != READ_ONCE(a52_p430_dma_ctrl))
		return;
	a52_p430_dma_sample(ctrl, point);
}
EXPORT_SYMBOL_GPL(a52_p430_dma_event);

EXPORT_SYMBOL_GPL(a52_p430_dma_triggered);

static void a52_p430_tcs_snapshot(struct a52_p430_dma_image *im)
{
	u32 t, n;
	im->child_cfg = a52_p424_r(a52_p424_rsc_drv, 0x0c);
	/* drv-id 0 occupies the low six bits; Phase427 incorrectly shifted by 6. */
	im->num_tcs = im->child_cfg & 0x3fU;
	im->ncpt = (im->child_cfg >> 27) & 0x1fU;
	for (t = 0; t < min_t(u32, im->num_tcs, 2U); t++) {
		u32 base = A52_P427_TCS_BASE + t * A52_P427_TCS_STRIDE;
		im->tcs[t].wait = a52_p424_r(a52_p424_rsc_drv, base + 0x10);
		im->tcs[t].control = a52_p424_r(a52_p424_rsc_drv, base + 0x14);
		im->tcs[t].status = a52_p424_r(a52_p424_rsc_drv, base + 0x18);
		im->tcs[t].enable = a52_p424_r(a52_p424_rsc_drv, base + 0x1c);
		for (n = 0; n < min_t(u32, im->ncpt, A52_P430_TCS_CMDS); n++) {
			u32 c = base + 0x30 + n * A52_P427_CMD_STRIDE;
			im->tcs[t].cmd[n].addr = a52_p424_r(a52_p424_rsc_drv, c + 0x04);
			im->tcs[t].cmd[n].data = a52_p424_r(a52_p424_rsc_drv, c + 0x08);
			im->tcs[t].cmd[n].status = a52_p424_r(a52_p424_rsc_drv, c + 0x0c);
		}
	}
}

static void a52_p430_clock_snapshot(struct a52_p430_dma_image *im)
{
	static const u32 disp_off[14] = {
		0x100c,0x1010,0x102c,0x1030,0x1034,0x104c,
		0x1064,0x1068,0x107c,0x1080,0x10c4,0x10c8,0x10e0,0x10e4,
	};
	u32 i;
	for (i = 0; i < ARRAY_SIZE(disp_off); i++)
		im->clk[i] = a52_p424_r(a52_p424_dispcc, disp_off[i]);
	im->clk[14] = a52_p424_r(a52_p424_gcc, 0x1700c);
	im->clk[15] = a52_p424_r(a52_p424_gcc, 0x1701c);
	im->clk[16] = a52_p424_r(a52_p427_hf0_gcc, 0x40);
	im->clk[17] = a52_p424_r(a52_p427_hf0_gcc, 0x44);
	im->clk[18] = a52_p424_r(a52_p424_rsc_drv, 0x0404);
	im->clk[19] = a52_p424_r(a52_p424_rsc_drv, 0x0408);
	im->pwr[0] = a52_p424_r(a52_p424_dispcc, 0x1004);
	im->pwr[1] = a52_p424_r(a52_p424_dispcc, 0x2004);
	im->pwr[2] = a52_p424_r(a52_p424_dispcc, 0x2008);
	im->pwr[3] = a52_p424_r(a52_p424_dispcc, 0x200c);
}

static void a52_p430_persist(void)
{
	void *d0, *d1, *d2;
	if (!a52_p430_dma_sideband)
		return;
	a52_p430_dma.capture_ns = ktime_get_boottime_ns();
	a52_p430_dma.crc32c = a52_p430_crc32c(&a52_p430_dma,
		offsetof(struct a52_p430_dma_image, crc32c));
	a52_p430_dma.commit = A52_P430_DMA_COMMIT;
	a52_p430_dma.version = A52_P430_DMA_VERSION;
	d0 = a52_p430_dma_sideband;
	d1 = (u8 *)a52_p430_dma_sideband + A52_P430_DMA_COPY_BYTES;
	d2 = (u8 *)a52_p430_dma_sideband + 2U * A52_P430_DMA_COPY_BYTES;
	memset(d0, 0, A52_P430_DMA_COPY_BYTES);
	memset(d1, 0, A52_P430_DMA_COPY_BYTES);
	memset(d2, 0, A52_P430_DMA_COPY_BYTES);
	memcpy(d0, &a52_p430_dma, sizeof(a52_p430_dma));
	memcpy(d1, &a52_p430_dma, sizeof(a52_p430_dma));
	memcpy(d2, &a52_p430_dma, sizeof(a52_p430_dma));
	wmb();
	__flush_dcache_area(d0, A52_P430_DMA_COPY_BYTES);
	__flush_dcache_area(d1, A52_P430_DMA_COPY_BYTES);
	__flush_dcache_area(d2, A52_P430_DMA_COPY_BYTES);
	dsb(sy);
}

void a52_p430_dma_finalize(struct dsi_ctrl_hw *ctrl, u64 iova,
			   u32 irq_trig, int wait_ret)
{
	int sr;
	if (!atomic_read(&a52_p430_dma_armed) || !ctrl)
		return;
	a52_p430_dma_sample(ctrl, 10U);
	a52_p430_dma.iova = iova;
	a52_p430_dma.wait_ret = (u32)wait_ret;
	a52_p430_dma.irq_trig = irq_trig;
	a52_p430_tcs_snapshot(&a52_p430_dma);
	a52_p430_clock_snapshot(&a52_p430_dma);
	sr = a52_p430_smmu_forensic(iova, &a52_p430_dma.smmu,
		sizeof(a52_p430_dma.smmu));
	a52_p430_persist();
	a52_ackfr_record("P430 D n=%u ret=%d irq=%u raw=%x bad=%u sr=%d",
		a52_p430_dma.sample_count, wait_ret, irq_trig,
		a52_p430_dma.sample_count ?
		a52_p430_dma.sample[a52_p430_dma.sample_count - 1U].int_ctrl : 0U,
		a52_p430_dma.bad_iova, sr);
	a52_ackfr_record("P430 M sw=%llx hw=%llx par=%llx at=%u fs=%x gf=%x",
		(unsigned long long)a52_p430_dma.smmu.sw_phys,
		(unsigned long long)a52_p430_dma.smmu.hw_phys,
		(unsigned long long)a52_p430_dma.smmu.par,
		a52_p430_dma.smmu.atos_timeout,
		a52_p430_dma.smmu.fsr, a52_p430_dma.smmu.gfsr);
	atomic_set(&a52_p430_dma_armed, 0);
}
EXPORT_SYMBOL_GPL(a52_p430_dma_finalize);

static int __init a52_p430_dma_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_p430_dma_image) > A52_P430_DMA_COPY_BYTES);
	a52_p430_dma_sideband = memremap(A52_P430_DMA_PHYS,
		A52_P430_DMA_BYTES, MEMREMAP_WB);
	if (a52_p430_dma_sideband) {
		memset(a52_p430_dma_sideband, 0, A52_P430_DMA_BYTES);
		wmb();
		__flush_dcache_area(a52_p430_dma_sideband, A52_P430_DMA_BYTES);
	}
	return 0;
}
subsys_initcall(a52_p430_dma_init);




/* A52_PHASE293_GKI_DMA_DONE_HW_REFERENCE_V1 */
extern bool a52_p293_gdm_trace_active(void);
extern void a52_ackfr_record(const char *fmt, ...);
extern void a52_p307_phy_snapshot(unsigned int index, unsigned int point);

/* A52_PHASE307_V3_PHY_CLOCKLANE_CORRELATION_V1
 * q0 = immediately before SW trigger, q1 = immediately after SW trigger.
 * Read-only controller + real v3 PHY correlation for exact F0 5A 5A only.
 */
/* A52_PHASE336_QSMMUV500_HF0_TBU_STATE_RECORDER_V1 */
extern void a52_p336_qsmmuv500_snapshot(u32 *seen, u32 *sacr_before,
						u32 *sacr_after, u32 *sacr_current,
						u32 *tbu_valid, u32 *tbu_status);

/* A52_PHASE316_GKI_F0_LAUNCH_FAULT_WINDOW_RECORDER_V1
 * Exact-F0 q0/q1/q2 read-only fault window. No ack/clear/write/delay/clock
 * action is added. DSI_DISP_CC_R32() is intentionally used here because it
 * reads ctrl->disp_cc_base (the DSI-specific disp_cc_base resource), not the
 * global Lagoon DISP_CC regmap whose offset 0 is PLL0.
 */
void a52_p316_fault_snapshot(struct dsi_ctrl_hw *ctrl, unsigned int point)
{
	u32 misc = 0xffffffff;
	u32 ck;

	if (!a52_p293_gdm_trace_active() || !ctrl || !ctrl->base)
		return;
	if (ctrl->disp_cc_base)
		misc = DSI_DISP_CC_R32(ctrl, 0x0);
	ck = DSI_R32(ctrl, DSI_CLK_STATUS);

	a52_ackfr_record("P276 316C q=%u st=%x fs=%x ck=%x ln=%x in=%x em=%x", point,
		DSI_R32(ctrl, DSI_STATUS), DSI_R32(ctrl, DSI_FIFO_STATUS), ck,
		DSI_R32(ctrl, DSI_LANE_STATUS), DSI_R32(ctrl, DSI_INT_CTRL),
		DSI_R32(ctrl, DSI_ERR_INT_MASK0));
	a52_ackfr_record("P276 316E q=%u ack=%x to=%x pe=%x ct=%x cc=%x lc=%x", point,
		DSI_R32(ctrl, DSI_ACK_ERR_STATUS), DSI_R32(ctrl, DSI_TIMEOUT_STATUS),
		DSI_R32(ctrl, DSI_DLN0_PHY_ERR), DSI_R32(ctrl, DSI_CTRL),
		DSI_R32(ctrl, DSI_CLK_CTRL), DSI_R32(ctrl, DSI_LANE_CTRL));
	a52_ackfr_record("P276 316D q=%u dc=%x df=%x of=%x le=%x sw=%x tr=%x", point,
		DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL),
		DSI_R32(ctrl, DSI_DMA_FIFO_CTRL), DSI_R32(ctrl, DSI_DMA_CMD_OFFSET),
		DSI_R32(ctrl, DSI_DMA_CMD_LENGTH),
		DSI_R32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER),
		DSI_R32(ctrl, DSI_TRIG_CTRL));
	a52_ackfr_record("P276 316M q=%u m=%x b0=%u b5=%u b7=%u b9=%u", point,
		misc, (misc >> 0) & 1, (misc >> 5) & 1,
		(misc >> 7) & 1, (misc >> 9) & 1);
	a52_ackfr_record("P276 316K q=%u ck=%x b7=%u b10=%u b12=%u b16=%u b23=%u", point,
		ck, (ck >> 7) & 1, (ck >> 10) & 1, (ck >> 12) & 1,
		(ck >> 16) & 1, (ck >> 23) & 1);

	{
		u32 a52_p336_seen = 0;
		u32 a52_p336_before = 0xffffffff;
		u32 a52_p336_after = 0xffffffff;
		u32 a52_p336_current = 0xffffffff;
		u32 a52_p336_tbu_valid = 0;
		u32 a52_p336_tbu_status = 0xffffffff;

		a52_p336_qsmmuv500_snapshot(&a52_p336_seen,
			&a52_p336_before, &a52_p336_after, &a52_p336_current,
			&a52_p336_tbu_valid, &a52_p336_tbu_status);
		a52_ackfr_record("P276 336A q=%u v=%u sb=%x sa=%x sc=%x tv=%u ts=%x",
			point, a52_p336_seen, a52_p336_before, a52_p336_after,
			a52_p336_current, a52_p336_tbu_valid, a52_p336_tbu_status);
	}
}

/* A52_PHASE319_DSI_SIXPOINT_TEMPORAL_OBSERVER_V1
 * A52_PHASE329_DSI_DEBUGBUS_SELECTOR_READBACK_V1
 * A52_PHASE330_DSI_EARLY_BREADCRUMB_RECORDER_V1
 * Matched Golden/GKI exact-F0 temporal observer for six raw Qualcomm DSI
 * debug-bus selectors identified by the Phase317 post-outcome comparison.
 * q0 is the last observer immediately before SW_TRIGGER, q1 is the first
 * observer immediately after SW_TRIGGER, and q2 is after completion outcome.
 * Only DSI_DEBUG_BUS_CTL is written, and the original selector is restored.
 * No clock/PHY/reset/regulator/delay/retry/repair behavior is introduced.
 */
/* A52_PHASE345_DMA_US_FRONTIER_V1
 *
 * High-resolution exact-F0 command-DMA observer matched to the successful
 * Phase344G TouchGrass capture.
 *
 * Golden established:
 *   p1 @ +18.802 us : STATUS=3, INT_CTRL low bit already asserted
 *   p2 @ +35.625 us : STATUS=0
 *   DMA_DONE ISR @ +46.042 us
 *
 * The old 0x8027c3/0x8037c3 difference is NOT DSI_CLK_STATUS.  It is
 * DSI_DEBUG_BUS_STATUS with selector 0x0171.  Therefore Phase345 keeps only
 * selector 0x0171 selected across the short p0..p6 burst, records the same
 * direct register set used by Phase344G, then restores the original selector.
 *
 * Samples stay in RAM during the transaction and are emitted into the retained
 * P276 recorder only at the existing exact-F0 200 ms timeout, immediately
 * before Phase280 freezes the evidence.  No printk occurs in the burst.
 */
#define A52_P345_MAX 8U

struct a52_p345_sample {
	u64 ns;
	u32 point;
	u32 status;
	u32 fifo;
	u32 clk_ctrl;
	u32 clk_status;
	u32 int_ctrl;
	u32 lane_status;
	u32 dma_ctrl;
	u32 dma_offset;
	u32 dma_length;
	u32 sw_trigger;
	u32 trig_ctrl;
	u32 ack_err;
	u32 timeout;
	u32 phy_err;
	u32 axi2ahb;
	u32 dbg171;
};

static struct a52_p345_sample a52_p345_samples[A52_P345_MAX];
static atomic_t a52_p345_state = ATOMIC_INIT(0); /* 0 idle, 1 burst, 2 captured */
static atomic_t a52_p345_count = ATOMIC_INIT(0);
static u32 a52_p345_saved_dbg_ctl;

/* A52_PHASE409_RAM1M_DEBUG2M_DSI_V1
 *
 * Focused two-tier DSI instrumentation:
 *
 *   HOT:  1 MiB non-wrapping binary event buffer in ordinary RAM.
 *         No block I/O, no printk, no R48 dependency in the Phase409 path.
 *
 *   COLD: one 2 MiB CRC32C-protected image written after the transaction to
 *         Samsung debug partition offset 0x800000..0x9fffff.
 *
 * The complete 1 MiB hot buffer is copied byte-for-byte into the cold image.
 * Phase345's microsecond samples are also copied into the cold summary area.
 */
#define A52_P409_RAM_BYTES          0x00100000U
#define A52_P409_DISK_BYTES         0x00200000U
#define A52_P409_DEBUG_OFFSET       0x00800000ULL
#define A52_P409_HEADER_BYTES       SZ_4K
#define A52_P409_HOT_DISK_OFFSET    A52_P409_HEADER_BYTES
#define A52_P409_COLD_OFFSET        (A52_P409_HOT_DISK_OFFSET + A52_P409_RAM_BYTES)
#define A52_P409_EVENT_BYTES        96U
#define A52_P409_EVENT_CAPACITY     (A52_P409_RAM_BYTES / A52_P409_EVENT_BYTES)
#define A52_P409_BIO_PAGES          128U
#define A52_P409_BIO_BYTES          (A52_P409_BIO_PAGES * PAGE_SIZE)
#define A52_P409_MAGIC              0x3930344953443241ULL /* A2DSI409 */
#define A52_P409_EVENT_COMMIT       0x409e0de5U
#define A52_P409_IMAGE_COMMIT       0x409c0de5U
#define A52_P409_VERSION            1U

enum a52_p409_stage {
	A52_P409_MATCH = 1,
	A52_P409_IRQ_PRE = 2,
	A52_P409_IRQ_POST = 3,
	A52_P409_DMA_PROGRAM = 4,
	A52_P409_TRIGGER_POST = 5,
	A52_P409_WAIT_ENTER = 6,
	A52_P409_ISR = 7,
	A52_P409_WAIT_EXIT = 8,
	A52_P409_FALLBACK = 9,
	A52_P409_FINAL = 10,
};

struct a52_p409_event {
	u64 ns;
	u32 seq;
	u16 stage;
	u16 cpu;
	u32 status;
	u32 fifo;
	u32 clk_ctrl;
	u32 clk_status;
	u32 int_ctrl;
	u32 lane_status;
	u32 dma_ctrl;
	u32 dma_offset;
	u32 dma_length;
	u32 sw_trigger;
	u32 trig_ctrl;
	u32 ack_err;
	u32 timeout;
	u32 phy_err;
	u32 axi2ahb;
	u32 dbg171;
	u32 aux0;
	u32 aux1;
	u32 reserved0;
	u32 commit;
} __packed;

struct a52_p409_header {
	u64 magic;
	u32 version;
	u32 phase;
	u64 capture_ns;
	u32 ram_bytes;
	u32 event_bytes;
	u32 event_capacity;
	u32 event_count;
	u32 event_dropped;
	u32 wait_ret;
	u32 dma_irq_trig;
	u32 phase345_count;
	u32 final_status;
	u32 final_fifo;
	u32 final_clk_ctrl;
	u32 final_clk_status;
	u32 final_int_ctrl;
	u32 final_lane_status;
	u32 final_dma_ctrl;
	u32 final_dma_offset;
	u32 final_dma_length;
	u32 final_sw_trigger;
	u32 final_trig_ctrl;
	u32 final_ack_err;
	u32 final_timeout;
	u32 final_phy_err;
	u32 final_axi2ahb;
	u32 final_dbg171;
	u32 writer_state;
	u32 reserved[31];
} __packed;

struct a52_p409_cold {
	u32 phase345_count;
	u32 reserved0;
	struct a52_p345_sample phase345[A52_P345_MAX];
} __packed;

static u32 a52_p409_crc32c(const void *buffer, size_t len)
{
	const u8 *bytes = buffer;
	u32 crc = ~0U;
	size_t index;
	unsigned int bit;

	for (index = 0; index < len; index++) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
				((crc & 1U) ? 0x82f63b78U : 0U);
	}
	return ~crc;
}

static struct a52_p409_event *a52_p409_hot;
static u8 *a52_p409_disk;
static atomic_t a52_p409_index = ATOMIC_INIT(0);
static atomic_t a52_p409_dropped = ATOMIC_INIT(0);
static atomic_t a52_p409_frozen = ATOMIC_INIT(0);
static atomic_t a52_p409_writer_state = ATOMIC_INIT(0);
static u32 a52_p409_wait_ret;
static u32 a52_p409_dma_irq_trig;
static struct a52_p409_event a52_p409_final_snapshot;

void a52_p409_init_buffers(void)
{
	BUILD_BUG_ON(sizeof(struct a52_p409_event) != A52_P409_EVENT_BYTES);
	BUILD_BUG_ON(A52_P409_HOT_DISK_OFFSET + A52_P409_RAM_BYTES >
		     A52_P409_DISK_BYTES);
	BUILD_BUG_ON(A52_P409_COLD_OFFSET + sizeof(struct a52_p409_cold) >
		     A52_P409_DISK_BYTES - 8U);
	BUILD_BUG_ON(PAGE_SIZE != 4096);

	if (!READ_ONCE(a52_p409_hot))
		a52_p409_hot = vzalloc(A52_P409_RAM_BYTES);
	if (!READ_ONCE(a52_p409_disk))
		a52_p409_disk = vzalloc(A52_P409_DISK_BYTES);
}

void a52_p409_hot_record(struct dsi_ctrl_hw *ctrl, u16 stage,
			 u32 aux0, u32 aux1)
{
	struct a52_p409_event *e;
	int index;

	if (!READ_ONCE(a52_p409_hot) || atomic_read(&a52_p409_frozen))
		return;
	if (!ctrl || !ctrl->base)
		return;

	index = atomic_inc_return(&a52_p409_index) - 1;
	if (index < 0 || index >= A52_P409_EVENT_CAPACITY) {
		atomic_inc(&a52_p409_dropped);
		return;
	}

	e = &a52_p409_hot[index];
	memset(e, 0, sizeof(*e));
	e->ns = ktime_get_ns();
	e->seq = (u32)index + 1U;
	e->stage = stage;
	e->cpu = (u16)raw_smp_processor_id();
	e->status = DSI_R32(ctrl, DSI_STATUS);
	e->fifo = DSI_R32(ctrl, DSI_FIFO_STATUS);
	e->clk_ctrl = DSI_R32(ctrl, DSI_CLK_CTRL);
	e->clk_status = DSI_R32(ctrl, DSI_CLK_STATUS);
	e->int_ctrl = DSI_R32(ctrl, DSI_INT_CTRL);
	e->lane_status = DSI_R32(ctrl, DSI_LANE_STATUS);
	e->dma_ctrl = DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL);
	e->dma_offset = DSI_R32(ctrl, DSI_DMA_CMD_OFFSET);
	e->dma_length = DSI_R32(ctrl, DSI_DMA_CMD_LENGTH);
	e->sw_trigger = DSI_R32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER);
	e->trig_ctrl = DSI_R32(ctrl, DSI_TRIG_CTRL);
	e->ack_err = DSI_R32(ctrl, DSI_ACK_ERR_STATUS);
	e->timeout = DSI_R32(ctrl, DSI_TIMEOUT_STATUS);
	e->phy_err = DSI_R32(ctrl, DSI_DLN0_PHY_ERR);
	e->axi2ahb = DSI_R32(ctrl, DSI_AXI2AHB_CTRL);
	e->dbg171 = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);
	e->aux0 = aux0;
	e->aux1 = aux1;
	wmb();
	e->commit = A52_P409_EVENT_COMMIT;
}

static int a52_p409_submit_chunk(struct block_device *bdev,
				 size_t offset, size_t bytes)
{
	struct bio *bio;
	unsigned int pages;
	unsigned int i;
	int added;
	int rc;

	if (!bdev || !a52_p409_disk || !bytes || (bytes & (PAGE_SIZE - 1U)))
		return -EINVAL;

	pages = bytes / PAGE_SIZE;
	if (pages > A52_P409_BIO_PAGES)
		return -EINVAL;

	bio = bio_alloc(GFP_KERNEL, pages);
	if (!bio)
		return -ENOMEM;

	bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector =
		(sector_t)((A52_P409_DEBUG_OFFSET + offset) >> 9);
	bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_SYNC | REQ_FUA);

	for (i = 0; i < pages; i++) {
		void *addr = a52_p409_disk + offset + (size_t)i * PAGE_SIZE;
		struct page *page = vmalloc_to_page(addr);

		if (!page) {
			bio_put(bio);
			return -EFAULT;
		}
		added = bio_add_page(bio, page, PAGE_SIZE, 0);
		if (added != PAGE_SIZE) {
			bio_put(bio);
			return -EIO;
		}
	}

	rc = submit_bio_wait(bio);
	bio_put(bio);
	return rc;
}

static void a52_p409_write_workfn(struct work_struct *work);
static DECLARE_WORK(a52_p409_write_work, a52_p409_write_workfn);

static void a52_p409_build_image(void)
{
	struct a52_p409_header *h;
	struct a52_p409_cold *cold;
	unsigned int n;
	u32 crc;
	u32 commit = A52_P409_IMAGE_COMMIT;

	memset(a52_p409_disk, 0, A52_P409_DISK_BYTES);

	h = (struct a52_p409_header *)a52_p409_disk;
	h->magic = A52_P409_MAGIC;
	h->version = A52_P409_VERSION;
	h->phase = 413U;
	h->capture_ns = ktime_get_boottime_ns();
	h->ram_bytes = A52_P409_RAM_BYTES;
	h->event_bytes = A52_P409_EVENT_BYTES;
	h->event_capacity = A52_P409_EVENT_CAPACITY;
	h->event_count = min_t(u32, (u32)atomic_read(&a52_p409_index),
			      A52_P409_EVENT_CAPACITY);
	h->event_dropped = (u32)atomic_read(&a52_p409_dropped);
	h->wait_ret = a52_p409_wait_ret;
	h->dma_irq_trig = a52_p409_dma_irq_trig;
	h->writer_state = (u32)atomic_read(&a52_p409_writer_state);

	h->final_status = a52_p409_final_snapshot.status;
	h->final_fifo = a52_p409_final_snapshot.fifo;
	h->final_clk_ctrl = a52_p409_final_snapshot.clk_ctrl;
	h->final_clk_status = a52_p409_final_snapshot.clk_status;
	h->final_int_ctrl = a52_p409_final_snapshot.int_ctrl;
	h->final_lane_status = a52_p409_final_snapshot.lane_status;
	h->final_dma_ctrl = a52_p409_final_snapshot.dma_ctrl;
	h->final_dma_offset = a52_p409_final_snapshot.dma_offset;
	h->final_dma_length = a52_p409_final_snapshot.dma_length;
	h->final_sw_trigger = a52_p409_final_snapshot.sw_trigger;
	h->final_trig_ctrl = a52_p409_final_snapshot.trig_ctrl;
	h->final_ack_err = a52_p409_final_snapshot.ack_err;
	h->final_timeout = a52_p409_final_snapshot.timeout;
	h->final_phy_err = a52_p409_final_snapshot.phy_err;
	h->final_axi2ahb = a52_p409_final_snapshot.axi2ahb;
	h->final_dbg171 = a52_p409_final_snapshot.dbg171;

	memcpy(a52_p409_disk + A52_P409_HOT_DISK_OFFSET,
	       a52_p409_hot, A52_P409_RAM_BYTES);

	cold = (struct a52_p409_cold *)(a52_p409_disk + A52_P409_COLD_OFFSET);
	n = (unsigned int)atomic_read(&a52_p345_count);
	if (n > A52_P345_MAX)
		n = A52_P345_MAX;
	cold->phase345_count = n;
	h->phase345_count = n;
	if (n)
		memcpy(cold->phase345, a52_p345_samples,
		       n * sizeof(cold->phase345[0]));

	crc = a52_p409_crc32c(a52_p409_disk, A52_P409_DISK_BYTES - 8U);
	memcpy(a52_p409_disk + A52_P409_DISK_BYTES - 8U, &crc, sizeof(crc));
	memcpy(a52_p409_disk + A52_P409_DISK_BYTES - 4U, &commit,
	       sizeof(commit));
	wmb();
}

static void a52_p409_write_workfn(struct work_struct *work)
{
	struct block_device *bdev;
	dev_t devt = MKDEV(SCSI_DISK0_MAJOR, 8);
	size_t offset;
	int rc = 0;

	(void)work;
	if (!a52_p409_hot || !a52_p409_disk) {
		atomic_set(&a52_p409_writer_state, 3);
		return;
	}

	atomic_set(&a52_p409_writer_state, 1);
	a52_p409_build_image();

	bdev = blkdev_get_by_dev(devt, FMODE_WRITE, NULL);
	if (IS_ERR(bdev)) {
		atomic_set(&a52_p409_writer_state, 3);
		return;
	}

	for (offset = 0; offset < A52_P409_DISK_BYTES;
	     offset += A52_P409_BIO_BYTES) {
		rc = a52_p409_submit_chunk(bdev, offset, A52_P409_BIO_BYTES);
		if (rc)
			break;
	}
	if (!rc)
		rc = blkdev_issue_flush(bdev, GFP_KERNEL);
	blkdev_put(bdev, FMODE_WRITE);

	if (!rc)
		atomic_set(&a52_p409_writer_state, 2);
	else
		atomic_set(&a52_p409_writer_state, 3);
}

void a52_p409_finalize_and_schedule(struct dsi_ctrl_hw *ctrl,
				    u32 wait_ret, u32 dma_irq_trig)
{
	int index;

	if (!a52_p409_hot || !a52_p409_disk || !ctrl || !ctrl->base)
		return;
	if (atomic_cmpxchg(&a52_p409_frozen, 0, 1) != 0)
		return;

	/* Final state is captured once; no further hot-buffer writes follow. */
	memset(&a52_p409_final_snapshot, 0, sizeof(a52_p409_final_snapshot));
	a52_p409_final_snapshot.ns = ktime_get_ns();
	a52_p409_final_snapshot.stage = A52_P409_FINAL;
	a52_p409_final_snapshot.cpu = (u16)raw_smp_processor_id();
	a52_p409_final_snapshot.status = DSI_R32(ctrl, DSI_STATUS);
	a52_p409_final_snapshot.fifo = DSI_R32(ctrl, DSI_FIFO_STATUS);
	a52_p409_final_snapshot.clk_ctrl = DSI_R32(ctrl, DSI_CLK_CTRL);
	a52_p409_final_snapshot.clk_status = DSI_R32(ctrl, DSI_CLK_STATUS);
	a52_p409_final_snapshot.int_ctrl = DSI_R32(ctrl, DSI_INT_CTRL);
	a52_p409_final_snapshot.lane_status = DSI_R32(ctrl, DSI_LANE_STATUS);
	a52_p409_final_snapshot.dma_ctrl = DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL);
	a52_p409_final_snapshot.dma_offset = DSI_R32(ctrl, DSI_DMA_CMD_OFFSET);
	a52_p409_final_snapshot.dma_length = DSI_R32(ctrl, DSI_DMA_CMD_LENGTH);
	a52_p409_final_snapshot.sw_trigger = DSI_R32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER);
	a52_p409_final_snapshot.trig_ctrl = DSI_R32(ctrl, DSI_TRIG_CTRL);
	a52_p409_final_snapshot.ack_err = DSI_R32(ctrl, DSI_ACK_ERR_STATUS);
	a52_p409_final_snapshot.timeout = DSI_R32(ctrl, DSI_TIMEOUT_STATUS);
	a52_p409_final_snapshot.phy_err = DSI_R32(ctrl, DSI_DLN0_PHY_ERR);
	a52_p409_final_snapshot.axi2ahb = DSI_R32(ctrl, DSI_AXI2AHB_CTRL);
	a52_p409_final_snapshot.dbg171 = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);
	a52_p409_final_snapshot.aux0 = wait_ret;
	a52_p409_final_snapshot.aux1 = dma_irq_trig;
	a52_p409_final_snapshot.commit = A52_P409_EVENT_COMMIT;

	index = atomic_read(&a52_p409_index);
	a52_p409_final_snapshot.seq =
		(index < A52_P409_EVENT_CAPACITY) ? (u32)index + 1U : 0U;
	a52_p409_wait_ret = wait_ret;
	a52_p409_dma_irq_trig = dma_irq_trig;
	schedule_work(&a52_p409_write_work);
}


/* A52_PHASE407_SAMSUNG_DEBUG_DSI_V1
 *
 * First Samsung debug-partition experiment:
 *   - ONE record only
 *   - ONE physical location only
 *   - CRC32C + commit marker
 *   - NO duplicate copy
 *   - NO R48 mirror
 *
 * The timing-sensitive exact-F0 burst remains RAM-only.  After the DMA wait
 * finishes, a compact binary snapshot is assembled in RAM and a workqueue
 * writes one 4 KiB page to the final page of Samsung's 10 MiB "debug"
 * partition (offset 0x9ff000).  The asynchronous block write therefore cannot
 * perturb SW_TRIGGER or the microsecond DMA_DONE observation window.
 */
#define A52_P407_RECORD_BYTES      4096U
#define A52_P407_DEBUG_OFFSET      0x009ff000ULL
#define A52_P407_MAGIC             0x3830344953443241ULL /* A2DSI408 */
#define A52_P407_COMMIT            0x408c0de5U
#define A52_P407_VERSION           1U

struct a52_p407_payload {
	u64 magic;
	u32 version;
	u32 phase;
	u64 capture_ns;
	u32 sample_count;
	u32 wait_ret;
	u32 dma_irq_trig;
	u32 final_status;
	u32 final_fifo;
	u32 final_clk_ctrl;
	u32 final_clk_status;
	u32 final_int_ctrl;
	u32 final_lane_status;
	u32 final_dma_ctrl;
	u32 final_dma_offset;
	u32 final_dma_length;
	u32 final_sw_trigger;
	u32 final_trig_ctrl;
	u32 final_ack_err;
	u32 final_timeout;
	u32 final_phy_err;
	u32 final_axi2ahb;
	u32 final_dbg171;
	struct a52_p345_sample samples[A52_P345_MAX];
} __packed;

static u8 a52_p407_page[A52_P407_RECORD_BYTES] __aligned(PAGE_SIZE);
static atomic_t a52_p407_state = ATOMIC_INIT(0); /* 0 idle, 1 captured, 2 written, 3 failed */

static u32 a52_p407_crc32c(const void *buffer, size_t len)
{
	const u8 *bytes = buffer;
	u32 crc = ~0U;
	size_t index;
	unsigned int bit;

	for (index = 0; index < len; index++) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
				((crc & 1U) ? 0x82f63b78U : 0U);
	}
	return ~crc;
}

static void a52_p407_write_workfn(struct work_struct *work);
static DECLARE_WORK(a52_p407_write_work, a52_p407_write_workfn);



static void a52_p407_write_workfn(struct work_struct *work)
{
	struct block_device *bdev;
	struct bio *bio;
	dev_t devt = MKDEV(SCSI_DISK0_MAJOR, 8);
	int added;
	int rc;

	(void)work;

	/*
	 * A52 debug partition is sda8.  Address it by dev_t directly so this
	 * path has no dependency on ueventd-created /dev nodes or by-name
	 * symlinks.  The sector is relative to the partition bdev.
	 */
	bdev = blkdev_get_by_dev(devt, FMODE_WRITE, NULL);
	if (IS_ERR(bdev)) {
		atomic_set(&a52_p407_state, 3);
		return;
	}

	bio = bio_alloc(GFP_KERNEL, 1);
	if (!bio) {
		blkdev_put(bdev, FMODE_WRITE);
		atomic_set(&a52_p407_state, 3);
		return;
	}

	bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector = (sector_t)(A52_P407_DEBUG_OFFSET >> 9);
	bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_SYNC | REQ_FUA);
	added = bio_add_page(bio, virt_to_page(a52_p407_page),
			     A52_P407_RECORD_BYTES,
			     offset_in_page(a52_p407_page));
	if (added != A52_P407_RECORD_BYTES) {
		bio_put(bio);
		blkdev_put(bdev, FMODE_WRITE);
		atomic_set(&a52_p407_state, 3);
		return;
	}

	rc = submit_bio_wait(bio);
	bio_put(bio);
	if (!rc)
		rc = blkdev_issue_flush(bdev, GFP_KERNEL);
	blkdev_put(bdev, FMODE_WRITE);

	if (!rc)
		atomic_set(&a52_p407_state, 2);
	else
		atomic_set(&a52_p407_state, 3);
}

void a52_p407_capture_and_schedule(struct dsi_ctrl_hw *ctrl,
				   u32 wait_ret, u32 dma_irq_trig)
{
	struct a52_p407_payload payload;
	unsigned int n;
	u32 crc;
	u32 commit = A52_P407_COMMIT;

	BUILD_BUG_ON(sizeof(payload) + 8U > A52_P407_RECORD_BYTES);
	if (!ctrl || !ctrl->base)
		return;
	if (atomic_cmpxchg(&a52_p407_state, 0, 1) != 0)
		return;

	memset(&payload, 0, sizeof(payload));
	payload.magic = A52_P407_MAGIC;
	payload.version = A52_P407_VERSION;
	payload.phase = 408U;
	payload.capture_ns = ktime_get_boottime_ns();
	n = (unsigned int)atomic_read(&a52_p345_count);
	if (n > A52_P345_MAX)
		n = A52_P345_MAX;
	payload.sample_count = n;
	payload.wait_ret = wait_ret;
	payload.dma_irq_trig = dma_irq_trig;

	payload.final_status = DSI_R32(ctrl, DSI_STATUS);
	payload.final_fifo = DSI_R32(ctrl, DSI_FIFO_STATUS);
	payload.final_clk_ctrl = DSI_R32(ctrl, DSI_CLK_CTRL);
	payload.final_clk_status = DSI_R32(ctrl, DSI_CLK_STATUS);
	payload.final_int_ctrl = DSI_R32(ctrl, DSI_INT_CTRL);
	payload.final_lane_status = DSI_R32(ctrl, DSI_LANE_STATUS);
	payload.final_dma_ctrl = DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL);
	payload.final_dma_offset = DSI_R32(ctrl, DSI_DMA_CMD_OFFSET);
	payload.final_dma_length = DSI_R32(ctrl, DSI_DMA_CMD_LENGTH);
	payload.final_sw_trigger = DSI_R32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER);
	payload.final_trig_ctrl = DSI_R32(ctrl, DSI_TRIG_CTRL);
	payload.final_ack_err = DSI_R32(ctrl, DSI_ACK_ERR_STATUS);
	payload.final_timeout = DSI_R32(ctrl, DSI_TIMEOUT_STATUS);
	payload.final_phy_err = DSI_R32(ctrl, DSI_DLN0_PHY_ERR);
	payload.final_axi2ahb = DSI_R32(ctrl, DSI_AXI2AHB_CTRL);
	payload.final_dbg171 = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);

	if (n)
		memcpy(payload.samples, a52_p345_samples,
		       n * sizeof(payload.samples[0]));

	memset(a52_p407_page, 0, sizeof(a52_p407_page));
	memcpy(a52_p407_page, &payload, sizeof(payload));
	crc = a52_p407_crc32c(a52_p407_page, A52_P407_RECORD_BYTES - 8U);
	memcpy(a52_p407_page + A52_P407_RECORD_BYTES - 8U, &crc, sizeof(crc));
	memcpy(a52_p407_page + A52_P407_RECORD_BYTES - 4U, &commit, sizeof(commit));
	wmb();

	schedule_work(&a52_p407_write_work);
}


/* A52_PHASE346_DMA_RAW_SIDEBAND_V1
 *
 * Phase345 proved that timeout-gated R48 export is not sufficient: if the
 * exact-F0 transaction succeeds, never reaches the timeout path, or its early
 * R48 records are later overwritten, the microsecond samples disappear.
 *
 * Reuse the dormant Phase342 sideband. Phase343 disables a52_r342_start(), so
 * 0xB1BFA000..0xB1BFC000 has no active writer in this lineage.
 *
 * Phase345 still captures p0..p6 into ordinary RAM with no cache maintenance.
 * Only AFTER p6 and DEBUG_BUS_CTL restore do we mirror+flush those samples here,
 * preserving the timing of the measured burst itself.
 */
#define A52_P346_SIDEBAND_PHYS  0xB1BFA000ULL
#define A52_P346_SIDEBAND_BYTES 0x2000U
#define A52_P346_COPY_BYTES     0x1000U
#define A52_P346_SLOT_BYTES     96U
#define A52_P346_MAGIC          0x3634335741524d44ULL
#define A52_P346_COMMIT         0x346c0de5U
#define A52_P346_INIT_POINT     0xffffffffU

struct a52_p346_slot {
	u64 magic;
	u64 ns;
	u32 index;
	u32 point;
	u32 status;
	u32 fifo;
	u32 clk_ctrl;
	u32 clk_status;
	u32 int_ctrl;
	u32 lane_status;
	u32 dma_ctrl;
	u32 dma_offset;
	u32 dma_length;
	u32 sw_trigger;
	u32 trig_ctrl;
	u32 ack_err;
	u32 timeout;
	u32 phy_err;
	u32 axi2ahb;
	u32 dbg171;
	u32 commit;
	u32 version;
};

static void *a52_p346_sideband;

static void a52_p346_write_slot(unsigned int slot_index,
				const struct a52_p346_slot *slot)
{
	void *dst0;
	void *dst1;
	unsigned int pos;

	if (!READ_ONCE(a52_p346_sideband) || !slot)
		return;
	pos = slot_index * A52_P346_SLOT_BYTES;
	if (pos + A52_P346_SLOT_BYTES > A52_P346_COPY_BYTES)
		return;

	dst0 = (u8 *)a52_p346_sideband + pos;
	dst1 = (u8 *)a52_p346_sideband + A52_P346_COPY_BYTES + pos;
	memcpy(dst0, slot, sizeof(*slot));
	memcpy(dst1, slot, sizeof(*slot));
	wmb();
	__flush_dcache_area(dst0, sizeof(*slot));
	__flush_dcache_area(dst1, sizeof(*slot));
}

void a52_p346_sideband_init(void)
{
	struct a52_p346_slot slot;

	BUILD_BUG_ON(sizeof(struct a52_p346_slot) != A52_P346_SLOT_BYTES);
	if (READ_ONCE(a52_p346_sideband))
		return;

	a52_p346_sideband = memremap(A52_P346_SIDEBAND_PHYS,
		A52_P346_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_p346_sideband)
		return;

	/* Erase stale Phase342/Phase346 payloads from earlier boots first. */
	memset(a52_p346_sideband, 0, A52_P346_SIDEBAND_BYTES);
	wmb();
	__flush_dcache_area(a52_p346_sideband, A52_P346_SIDEBAND_BYTES);

	memset(&slot, 0, sizeof(slot));
	slot.magic = A52_P346_MAGIC;
	slot.ns = ktime_get_ns();
	slot.index = 0xffffffffU;
	slot.point = A52_P346_INIT_POINT;
	slot.status = 1U; /* mapping initialized */
	slot.commit = A52_P346_COMMIT;
	slot.version = 1U;
	a52_p346_write_slot(0U, &slot);
}

static void a52_p346_persist_samples(void)
{
	unsigned int i;
	unsigned int n = (unsigned int)atomic_read(&a52_p345_count);

	if (!READ_ONCE(a52_p346_sideband))
		return;
	if (n > A52_P345_MAX)
		n = A52_P345_MAX;

	for (i = 0; i < n; i++) {
		const struct a52_p345_sample *s = &a52_p345_samples[i];
		struct a52_p346_slot slot;

		memset(&slot, 0, sizeof(slot));
		slot.magic = A52_P346_MAGIC;
		slot.ns = s->ns;
		slot.index = i;
		slot.point = s->point;
		slot.status = s->status;
		slot.fifo = s->fifo;
		slot.clk_ctrl = s->clk_ctrl;
		slot.clk_status = s->clk_status;
		slot.int_ctrl = s->int_ctrl;
		slot.lane_status = s->lane_status;
		slot.dma_ctrl = s->dma_ctrl;
		slot.dma_offset = s->dma_offset;
		slot.dma_length = s->dma_length;
		slot.sw_trigger = s->sw_trigger;
		slot.trig_ctrl = s->trig_ctrl;
		slot.ack_err = s->ack_err;
		slot.timeout = s->timeout;
		slot.phy_err = s->phy_err;
		slot.axi2ahb = s->axi2ahb;
		slot.dbg171 = s->dbg171;
		slot.commit = A52_P346_COMMIT;
		slot.version = 1U;
		/* Slot 0 is the init marker; samples start at slot 1. */
		a52_p346_write_slot(i + 1U, &slot);
	}
}

static void a52_p345_store(struct dsi_ctrl_hw *ctrl, unsigned int point)
{
	struct a52_p345_sample *s;
	unsigned int i;

	if (atomic_read(&a52_p345_state) != 1 || !ctrl || !ctrl->base)
		return;

	i = (unsigned int)atomic_inc_return(&a52_p345_count) - 1U;
	if (i >= A52_P345_MAX)
		return;

	s = &a52_p345_samples[i];
	memset(s, 0, sizeof(*s));
	s->ns = ktime_get_ns();
	s->point = point;
	s->status = DSI_R32(ctrl, DSI_STATUS);
	s->fifo = DSI_R32(ctrl, DSI_FIFO_STATUS);
	s->clk_ctrl = DSI_R32(ctrl, DSI_CLK_CTRL);
	s->clk_status = DSI_R32(ctrl, DSI_CLK_STATUS);
	s->int_ctrl = DSI_R32(ctrl, DSI_INT_CTRL);
	s->lane_status = DSI_R32(ctrl, DSI_LANE_STATUS);
	s->dma_ctrl = DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL);
	s->dma_offset = DSI_R32(ctrl, DSI_DMA_CMD_OFFSET);
	s->dma_length = DSI_R32(ctrl, DSI_DMA_CMD_LENGTH);
	s->sw_trigger = DSI_R32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER);
	s->trig_ctrl = DSI_R32(ctrl, DSI_TRIG_CTRL);
	s->ack_err = DSI_R32(ctrl, DSI_ACK_ERR_STATUS);
	s->timeout = DSI_R32(ctrl, DSI_TIMEOUT_STATUS);
	s->phy_err = DSI_R32(ctrl, DSI_DLN0_PHY_ERR);
	s->axi2ahb = DSI_R32(ctrl, DSI_AXI2AHB_CTRL);
	s->dbg171 = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);
}

static void __maybe_unused a52_p345_begin(struct dsi_ctrl_hw *ctrl)
{
	if (!a52_p293_gdm_trace_active() || !ctrl || !ctrl->base)
		return;
	if (atomic_cmpxchg(&a52_p345_state, 0, 1) != 0)
		return;

	atomic_set(&a52_p345_count, 0);
	a52_p345_saved_dbg_ctl = DSI_R32(ctrl, DSI_DEBUG_BUS_CTL);
	DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, 0x0171);
	wmb();
	a52_p345_store(ctrl, 0);
}

static void __maybe_unused a52_p345_end(struct dsi_ctrl_hw *ctrl)
{
	if (atomic_read(&a52_p345_state) != 1 || !ctrl || !ctrl->base)
		return;

	DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, a52_p345_saved_dbg_ctl);
	wmb();
	atomic_set(&a52_p345_state, 2);
	/* Persist only after the p0..p6 measurement burst is complete. */
	a52_p346_persist_samples();
}

void a52_p345_flush(struct dsi_ctrl_hw *ctrl)
{
	unsigned int i;
	unsigned int n = (unsigned int)atomic_read(&a52_p345_count);
	u64 base;

	if (atomic_read(&a52_p345_state) < 2 || !n)
		return;
	if (n > A52_P345_MAX)
		n = A52_P345_MAX;

	base = a52_p345_samples[0].ns;
	for (i = 0; i < n; i++) {
		const struct a52_p345_sample *s = &a52_p345_samples[i];
		a52_ackfr_record("P276 345R %x %x %llx %x %x %x %x",
			i, s->point, (unsigned long long)(s->ns - base),
			s->status, s->int_ctrl, s->clk_status, s->dbg171);
	}

	if (ctrl && ctrl->base)
		a52_ackfr_record("P276 345T %x %x %x %x %x %x", n,
			DSI_R32(ctrl, DSI_STATUS), DSI_R32(ctrl, DSI_INT_CTRL),
			DSI_R32(ctrl, DSI_ACK_ERR_STATUS),
			DSI_R32(ctrl, DSI_TIMEOUT_STATUS),
			DSI_R32(ctrl, DSI_DLN0_PHY_ERR));
}

static const u32 a52_p319_selectors[6] = {
	0x0171, 0x0181, 0x0191, 0x01a1, 0x01e1, 0x0211,
};

void a52_p319_debugbus_snapshot(struct dsi_ctrl_hw *ctrl, unsigned int point)
{
	u32 saved, restored_ctl, restored_status, i;
	u32 v[6], ctl[6];

	if (!a52_p293_gdm_trace_active() || !ctrl || !ctrl->base)
		return;

	a52_ackfr_record("P276 330A q=%u", point);
	saved = DSI_R32(ctrl, DSI_DEBUG_BUS_CTL);
	a52_ackfr_record("P276 330B q=%u c=%x", point, saved);
	for (i = 0; i < 6; i++) {
		DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, a52_p319_selectors[i]);
		wmb();
		ctl[i] = DSI_R32(ctrl, DSI_DEBUG_BUS_CTL);
		v[i] = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);
		a52_ackfr_record("P276 330C q=%u i=%u c=%x v=%x",
			point, i, ctl[i], v[i]);
	}
	DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, saved);
	wmb();
	restored_ctl = DSI_R32(ctrl, DSI_DEBUG_BUS_CTL);
	restored_status = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);
	a52_ackfr_record("P276 330D q=%u c=%x z=%x",
		point, restored_ctl, restored_status);

	a52_ackfr_record("P276 319B q=%u c=%x 171=%x 181=%x 191=%x 1a1=%x 1e1=%x 211=%x z=%x r=%x",
		point, saved, v[0], v[1], v[2], v[3], v[4], v[5], restored_status,
		restored_ctl);
	a52_ackfr_record("P276 329C q=%u a=%x b=%x c=%x d=%x e=%x f=%x",
		point, ctl[0], ctl[1], ctl[2], ctl[3], ctl[4], ctl[5]);
}

static void __maybe_unused a52_p307_hw_snapshot(struct dsi_ctrl_hw *ctrl, unsigned int point)
{
	if (!a52_p293_gdm_trace_active() || !ctrl || !ctrl->base)
		return;
	a52_ackfr_record("P276 307C q=%u st=%x ln=%x ck=%x cc=%x in=%x", point,
		DSI_R32(ctrl, DSI_STATUS), DSI_R32(ctrl, DSI_LANE_STATUS),
		DSI_R32(ctrl, DSI_CLK_STATUS), DSI_R32(ctrl, DSI_CLK_CTRL),
		DSI_R32(ctrl, DSI_INT_CTRL));
	a52_p316_fault_snapshot(ctrl, point);
	a52_p307_phy_snapshot(ctrl->index, point);
}

/* A52_PHASE445_RECOVERY_LADDER_V1 */
extern bool a52_p445_active(void);
extern void a52_p445_pre_trigger(struct dsi_ctrl_hw *ctrl);
extern void a52_p445_post_trigger(struct dsi_ctrl_hw *ctrl);

#define MMSS_MISC_CLAMP_REG_OFF           0x0014
#define DSI_CTRL_DYNAMIC_FORCE_ON         (0x23F|BIT(8)|BIT(9)|BIT(11)|BIT(21))
#define DSI_CTRL_CMD_MISR_ENABLE          BIT(28)
#define DSI_CTRL_VIDEO_MISR_ENABLE        BIT(16)
#define DSI_CTRL_DMA_LINK_SEL             (BIT(12)|BIT(13))
#define DSI_CTRL_MDP0_LINK_SEL            (BIT(20)|BIT(22))

/* Unsupported formats default to RGB888 */
static const u8 cmd_mode_format_map[DSI_PIXEL_FORMAT_MAX] = {
	0x6, 0x7, 0x8, 0x8, 0x0, 0x3, 0x4 };
static const u8 video_mode_format_map[DSI_PIXEL_FORMAT_MAX] = {
	0x0, 0x1, 0x2, 0x3, 0x3, 0x3, 0x3 };

/**
 * dsi_split_link_setup() - setup dsi split link configurations
 * @ctrl:             Pointer to the controller host hardware.
 * @cfg:              DSI host configuration that is common to both video and
 *                    command modes.
 */
static void dsi_split_link_setup(struct dsi_ctrl_hw *ctrl,
				struct dsi_host_common_cfg *cfg)
{
	u32 reg;

	if (!cfg->split_link.split_link_enabled)
		return;

	reg = DSI_R32(ctrl, DSI_SPLIT_LINK);

	/* DMA_LINK_SEL */
	reg &= ~(0x7 << 12);
	reg |= DSI_CTRL_DMA_LINK_SEL;

	/* MDP0_LINK_SEL */
	reg &= ~(0x7 << 20);
	reg |= DSI_CTRL_MDP0_LINK_SEL;

	/* EN */
	reg |= 0x1;

	/* DSI_SPLIT_LINK */
	DSI_W32(ctrl, DSI_SPLIT_LINK, reg);
	wmb(); /* make sure split link is asserted */
}

/**
 * dsi_setup_trigger_controls() - setup dsi trigger configurations
 * @ctrl:             Pointer to the controller host hardware.
 * @cfg:              DSI host configuration that is common to both video and
 *                    command modes.
 */
static void dsi_setup_trigger_controls(struct dsi_ctrl_hw *ctrl,
				       struct dsi_host_common_cfg *cfg)
{
	u32 reg = 0;
	const u8 trigger_map[DSI_TRIGGER_MAX] = {
		0x0, 0x2, 0x1, 0x4, 0x5, 0x6 };

	reg |= (cfg->te_mode == DSI_TE_ON_EXT_PIN) ? BIT(31) : 0;
	reg |= (trigger_map[cfg->dma_cmd_trigger] & 0x7);
	reg |= (trigger_map[cfg->mdp_cmd_trigger] & 0x7) << 4;
	DSI_W32(ctrl, DSI_TRIG_CTRL, reg);
}

/**
 * dsi_ctrl_hw_cmn_host_setup() - setup dsi host configuration
 * @ctrl:             Pointer to the controller host hardware.
 * @cfg:              DSI host configuration that is common to both video and
 *                    command modes.
 */
void dsi_ctrl_hw_cmn_host_setup(struct dsi_ctrl_hw *ctrl,
			       struct dsi_host_common_cfg *cfg)
{
	u32 reg_value = 0;

	dsi_setup_trigger_controls(ctrl, cfg);
	dsi_split_link_setup(ctrl, cfg);

	/* Setup T_CLK_PRE extend register */
	reg_value = DSI_R32(ctrl, DSI_TEST_PATTERN_GEN_VIDEO_ENABLE);
	if (cfg->t_clk_pre_extend)
		reg_value |= BIT(0);
	else
		reg_value &= ~BIT(0);
	DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_VIDEO_ENABLE, reg_value);

	/* Setup clocking timing controls */
	reg_value = ((cfg->t_clk_post & 0x3F) << 8);
	reg_value |= (cfg->t_clk_pre & 0x3F);
	DSI_W32(ctrl, DSI_CLKOUT_TIMING_CTRL, reg_value);

	/* EOT packet control */
	reg_value = cfg->append_tx_eot ? 1 : 0;
	reg_value |= (cfg->ignore_rx_eot ? (1 << 4) : 0);
	DSI_W32(ctrl, DSI_EOT_PACKET_CTRL, reg_value);

	/* Turn on dsi clocks */
	DSI_W32(ctrl, DSI_CLK_CTRL, 0x23F);

	/* Setup DSI control register */
	reg_value = DSI_R32(ctrl, DSI_CTRL);
	reg_value |= (cfg->en_crc_check ? BIT(24) : 0);
	reg_value |= (cfg->en_ecc_check ? BIT(20) : 0);
	reg_value |= BIT(8); /* Clock lane */
	reg_value |= ((cfg->data_lanes & DSI_DATA_LANE_3) ? BIT(7) : 0);
	reg_value |= ((cfg->data_lanes & DSI_DATA_LANE_2) ? BIT(6) : 0);
	reg_value |= ((cfg->data_lanes & DSI_DATA_LANE_1) ? BIT(5) : 0);
	reg_value |= ((cfg->data_lanes & DSI_DATA_LANE_0) ? BIT(4) : 0);

	DSI_W32(ctrl, DSI_CTRL, reg_value);

	if (cfg->phy_type == DSI_PHY_TYPE_CPHY)
		DSI_W32(ctrl, DSI_CPHY_MODE_CTRL, BIT(0));

	if (ctrl->phy_isolation_enabled)
		DSI_W32(ctrl, DSI_DEBUG_CTRL, BIT(28));
	DSI_CTRL_HW_DBG(ctrl, "Host configuration complete\n");
}

/**
 * phy_sw_reset() - perform a soft reset on the PHY.
 * @ctrl:        Pointer to the controller host hardware.
 */
void dsi_ctrl_hw_cmn_phy_sw_reset(struct dsi_ctrl_hw *ctrl)
{
	DSI_W32(ctrl, DSI_PHY_SW_RESET, BIT(24)|BIT(0));
	wmb(); /* make sure reset is asserted */
	udelay(1000);
	DSI_W32(ctrl, DSI_PHY_SW_RESET, 0x0);
	wmb(); /* ensure reset is cleared before waiting */
	udelay(100);

	DSI_CTRL_HW_DBG(ctrl, "phy sw reset done\n");
}

/**
 * soft_reset() - perform a soft reset on DSI controller
 * @ctrl:          Pointer to the controller host hardware.
 *
 * The video, command and controller engines will be disabled before the
 * reset is triggered and re-enabled after the reset is complete.
 *
 * If the reset is done while MDP timing engine is turned on, the video
 * enigne should be re-enabled only during the vertical blanking time.
 */
void dsi_ctrl_hw_cmn_soft_reset(struct dsi_ctrl_hw *ctrl)
{
	u32 reg = 0;
	u32 reg_ctrl = 0;

	/* Clear DSI_EN, VIDEO_MODE_EN, CMD_MODE_EN */
	reg_ctrl = DSI_R32(ctrl, DSI_CTRL);
	DSI_W32(ctrl, DSI_CTRL, reg_ctrl & ~0x7);
	wmb(); /* wait controller to be disabled before reset */

	/* Force enable PCLK, BYTECLK, AHBM_HCLK */
	reg = DSI_R32(ctrl, DSI_CLK_CTRL);
	DSI_W32(ctrl, DSI_CLK_CTRL, reg | DSI_CTRL_DYNAMIC_FORCE_ON);
	wmb(); /* wait for clocks to be enabled */

	/* Trigger soft reset */
	DSI_W32(ctrl, DSI_SOFT_RESET, 0x1);
	wmb(); /* wait for reset to assert before waiting */
	udelay(1);
	DSI_W32(ctrl, DSI_SOFT_RESET, 0x0);
	wmb(); /* ensure reset is cleared */

	/* Disable force clock on */
	DSI_W32(ctrl, DSI_CLK_CTRL, reg);
	wmb(); /* make sure clocks are restored */

	/* Re-enable DSI controller */
	DSI_W32(ctrl, DSI_CTRL, reg_ctrl);
	wmb(); /* make sure DSI controller is enabled again */
	DSI_CTRL_HW_DBG(ctrl, "ctrl soft reset done\n");
	SDE_EVT32(ctrl->index);
}

/**
 * setup_misr() - Setup frame MISR
 * @ctrl:	  Pointer to the controller host hardware.
 * @panel_mode:   CMD or VIDEO mode indicator
 * @enable:	  Enable/disable MISR.
 * @frame_count:  Number of frames to accumulate MISR.
 */
void dsi_ctrl_hw_cmn_setup_misr(struct dsi_ctrl_hw *ctrl,
			enum dsi_op_mode panel_mode,
			bool enable,
			u32 frame_count)
{
	u32 addr;
	u32 config = 0;

	if (panel_mode == DSI_OP_CMD_MODE) {
		addr = DSI_MISR_CMD_CTRL;
		if (enable)
			config = DSI_CTRL_CMD_MISR_ENABLE;
	} else {
		addr = DSI_MISR_VIDEO_CTRL;
		if (enable)
			config = DSI_CTRL_VIDEO_MISR_ENABLE;
		if (frame_count > 255)
			frame_count = 255;
		config |= frame_count << 8;
	}

	DSI_CTRL_HW_DBG(ctrl, "MISR ctrl: 0x%x\n", config);
	DSI_W32(ctrl, addr, config);
	wmb(); /* make sure MISR is configured */
}

/**
 * collect_misr() - Read frame MISR
 * @ctrl:	  Pointer to the controller host hardware.
 * @panel_mode:   CMD or VIDEO mode indicator
 */
u32 dsi_ctrl_hw_cmn_collect_misr(struct dsi_ctrl_hw *ctrl,
			enum dsi_op_mode panel_mode)
{
	u32 addr;
	u32 enabled;
	u32 misr = 0;

	if (panel_mode == DSI_OP_CMD_MODE) {
		addr = DSI_MISR_CMD_MDP0_32BIT;
		enabled = DSI_R32(ctrl, DSI_MISR_CMD_CTRL) &
				DSI_CTRL_CMD_MISR_ENABLE;
	} else {
		addr = DSI_MISR_VIDEO_32BIT;
		enabled = DSI_R32(ctrl, DSI_MISR_VIDEO_CTRL) &
				DSI_CTRL_VIDEO_MISR_ENABLE;
	}

	if (enabled)
		misr = DSI_R32(ctrl, addr);

	DSI_CTRL_HW_DBG(ctrl, "MISR enabled %x value: 0x%x\n", enabled, misr);
	return misr;
}

/**
 * set_timing_db() - enable/disable Timing DB register
 * @ctrl:          Pointer to controller host hardware.
 * @enable:        Enable/Disable flag.
 *
 * Enable or Disabe the Timing DB register.
 */
void dsi_ctrl_hw_cmn_set_timing_db(struct dsi_ctrl_hw *ctrl,
				     bool enable)
{
	if (enable)
		DSI_W32(ctrl, DSI_DSI_TIMING_DB_MODE, 0x1);
	else
		DSI_W32(ctrl, DSI_DSI_TIMING_DB_MODE, 0x0);

	wmb(); /* make sure timing db registers are set */
	DSI_CTRL_HW_DBG(ctrl, "ctrl timing DB set:%d\n", enable);
	SDE_EVT32(ctrl->index, enable);
}

/**
 * set_video_timing() - set up the timing for video frame
 * @ctrl:          Pointer to controller host hardware.
 * @mode:          Video mode information.
 *
 * Set up the video timing parameters for the DSI video mode operation.
 */
void dsi_ctrl_hw_cmn_set_video_timing(struct dsi_ctrl_hw *ctrl,
				     struct dsi_mode_info *mode)
{
	u32 reg = 0;
	u32 hs_start = 0;
	u32 hs_end, active_h_start, active_h_end, h_total, width = 0;
	u32 vs_start = 0, vs_end = 0;
	u32 vpos_start = 0, vpos_end, active_v_start, active_v_end, v_total;

	if (mode->dsc_enabled && mode->dsc) {
		width = mode->dsc->pclk_per_line;
		reg = mode->dsc->bytes_per_pkt << 16;
		reg |= (0x0b << 8);    /* dtype of compressed image */
		/*
		 * pkt_per_line:
		 * 0 == 1 pkt
		 * 1 == 2 pkt
		 * 2 == 4 pkt
		 * 3 pkt is not support
		 */
		if (mode->dsc->pkt_per_line == 4)
			reg |= (mode->dsc->pkt_per_line - 2) << 6;
		else
			reg |= (mode->dsc->pkt_per_line - 1) << 6;
		reg |= mode->dsc->eol_byte_num << 4;
		reg |= 1;
		DSI_W32(ctrl, DSI_VIDEO_COMPRESSION_MODE_CTRL, reg);
	} else {
		width = mode->h_active;
	}

	hs_end = mode->h_sync_width;
	active_h_start = mode->h_sync_width + mode->h_back_porch;
	active_h_end = active_h_start + width;
	h_total = (mode->h_sync_width + mode->h_back_porch + width +
		   mode->h_front_porch) - 1;

	vpos_end = mode->v_sync_width;
	active_v_start = mode->v_sync_width + mode->v_back_porch;
	active_v_end = active_v_start + mode->v_active;
	v_total = (mode->v_sync_width + mode->v_back_porch + mode->v_active +
		   mode->v_front_porch) - 1;

	reg = ((active_h_end & 0xFFFF) << 16) | (active_h_start & 0xFFFF);
	DSI_W32(ctrl, DSI_VIDEO_MODE_ACTIVE_H, reg);

	reg = ((active_v_end & 0xFFFF) << 16) | (active_v_start & 0xFFFF);
	DSI_W32(ctrl, DSI_VIDEO_MODE_ACTIVE_V, reg);

	reg = ((v_total & 0xFFFF) << 16) | (h_total & 0xFFFF);
	DSI_W32(ctrl, DSI_VIDEO_MODE_TOTAL, reg);

	reg = ((hs_end & 0xFFFF) << 16) | (hs_start & 0xFFFF);
	DSI_W32(ctrl, DSI_VIDEO_MODE_HSYNC, reg);

	reg = ((vs_end & 0xFFFF) << 16) | (vs_start & 0xFFFF);
	DSI_W32(ctrl, DSI_VIDEO_MODE_VSYNC, reg);

	reg = ((vpos_end & 0xFFFF) << 16) | (vpos_start & 0xFFFF);
	DSI_W32(ctrl, DSI_VIDEO_MODE_VSYNC_VPOS, reg);

	/* TODO: HS TIMER value? */
	DSI_W32(ctrl, DSI_HS_TIMER_CTRL, 0x3FD08);
	DSI_W32(ctrl, DSI_MISR_VIDEO_CTRL, 0x10100);
	DSI_W32(ctrl, DSI_DSI_TIMING_FLUSH, 0x1);
	DSI_CTRL_HW_DBG(ctrl, "ctrl video parameters updated\n");
	SDE_EVT32(v_total, h_total);
}

/**
 * setup_cmd_stream() - set up parameters for command pixel streams
 * @ctrl:              Pointer to controller host hardware.
 * @mode:              Pointer to mode information.
 * @h_stride:          Horizontal stride in bytes.
 * @vc_id:             stream_id
 *
 * Setup parameters for command mode pixel stream size.
 */
void dsi_ctrl_hw_cmn_setup_cmd_stream(struct dsi_ctrl_hw *ctrl,
				     struct dsi_mode_info *mode,
				     u32 h_stride,
				     u32 vc_id,
				     struct dsi_rect *roi)
{
	u32 width_final, stride_final;
	u32 height_final;
	u32 stream_total = 0, stream_ctrl = 0;
	u32 reg_ctrl = 0, reg_ctrl2 = 0, data = 0;

	if (roi && (!roi->w || !roi->h))
		return;

	if (mode->dsc_enabled && mode->dsc) {
		u32 reg = 0;
		u32 offset = 0;
		int pic_width, this_frame_slices, intf_ip_w;
		struct msm_display_dsc_info dsc;

		memcpy(&dsc, mode->dsc, sizeof(dsc));
		pic_width = roi ? roi->w : mode->h_active;
		this_frame_slices = pic_width / dsc.slice_width;
		intf_ip_w = this_frame_slices * dsc.slice_width;
		dsi_dsc_pclk_param_calc(&dsc, intf_ip_w);

		if (vc_id != 0)
			offset = 16;
		reg_ctrl = DSI_R32(ctrl, DSI_COMMAND_COMPRESSION_MODE_CTRL);
		reg_ctrl2 = DSI_R32(ctrl, DSI_COMMAND_COMPRESSION_MODE_CTRL2);
		width_final = dsc.pclk_per_line;
		stride_final = dsc.bytes_per_pkt;
		height_final = roi ? roi->h : mode->v_active;

		reg = 0x39 << 8;
		/*
		 * pkt_per_line:
		 * 0 == 1 pkt
		 * 1 == 2 pkt
		 * 2 == 4 pkt
		 * 3 pkt is not support
		 */
		if (dsc.pkt_per_line == 4)
			reg |= (dsc.pkt_per_line - 2) << 6;
		else
			reg |= (dsc.pkt_per_line - 1) << 6;
		reg |= dsc.eol_byte_num << 4;
		reg |= 1;

		reg_ctrl &= ~(0xFFFF << offset);
		reg_ctrl |= (reg << offset);
		reg_ctrl2 &= ~(0xFFFF << offset);
		reg_ctrl2 |= (dsc.bytes_in_slice << offset);

		DSI_CTRL_HW_DBG(ctrl, "reg_ctrl 0x%x reg_ctrl2 0x%x\n",
				reg_ctrl, reg_ctrl2);
	} else if (roi) {
		width_final = roi->w;
		stride_final = roi->w * 3;
		height_final = roi->h;
	} else {
		width_final = mode->h_active;
		stride_final = h_stride;
		height_final = mode->v_active;
	}

	/* HS Timer value */
	DSI_W32(ctrl, DSI_HS_TIMER_CTRL, 0x3FD08);

	stream_ctrl = (stride_final + 1) << 16;
	stream_ctrl |= (vc_id & 0x3) << 8;
	stream_ctrl |= 0x39; /* packet data type */

	DSI_W32(ctrl, DSI_COMMAND_COMPRESSION_MODE_CTRL, reg_ctrl);
	DSI_W32(ctrl, DSI_COMMAND_COMPRESSION_MODE_CTRL2, reg_ctrl2);

	DSI_W32(ctrl, DSI_COMMAND_MODE_MDP_STREAM0_CTRL, stream_ctrl);
	DSI_W32(ctrl, DSI_COMMAND_MODE_MDP_STREAM1_CTRL, stream_ctrl);

	stream_total = (height_final << 16) | width_final;
	DSI_W32(ctrl, DSI_COMMAND_MODE_MDP_STREAM0_TOTAL, stream_total);
	DSI_W32(ctrl, DSI_COMMAND_MODE_MDP_STREAM1_TOTAL, stream_total);

	if (ctrl->null_insertion_enabled) {
		/* enable null packet insertion */
		data = (vc_id << 1);
		data |= 0 << 16;
		data |= 0x1;
		DSI_W32(ctrl, DSI_COMMAND_MODE_NULL_INSERTION_CTRL, data);
	}

	DSI_CTRL_HW_DBG(ctrl, "stream_ctrl 0x%x stream_total 0x%x\n",
			stream_ctrl, stream_total);
}

/**
 * setup_avr() - set the AVR_SUPPORT_ENABLE bit in DSI_VIDEO_MODE_CTRL
 * @ctrl:          Pointer to controller host hardware.
 * @enable:        Controls whether this bit is set or cleared
 *
 * Set or clear the AVR_SUPPORT_ENABLE bit in DSI_VIDEO_MODE_CTRL.
 */
void dsi_ctrl_hw_cmn_setup_avr(struct dsi_ctrl_hw *ctrl, bool enable)
{
	u32 reg = DSI_R32(ctrl, DSI_VIDEO_MODE_CTRL);

	if (enable)
		reg |= BIT(29);
	else
		reg &= ~BIT(29);

	DSI_W32(ctrl, DSI_VIDEO_MODE_CTRL, reg);
	DSI_CTRL_HW_DBG(ctrl, "AVR %s\n", enable ? "enabled" : "disabled");
}

/**
 * video_engine_setup() - Setup dsi host controller for video mode
 * @ctrl:          Pointer to controller host hardware.
 * @common_cfg:    Common configuration parameters.
 * @cfg:           Video mode configuration.
 *
 * Set up DSI video engine with a specific configuration. Controller and
 * video engine are not enabled as part of this function.
 */
void dsi_ctrl_hw_cmn_video_engine_setup(struct dsi_ctrl_hw *ctrl,
				       struct dsi_host_common_cfg *common_cfg,
				       struct dsi_video_engine_cfg *cfg)
{
	u32 reg = 0;

	reg |= (cfg->last_line_interleave_en ? BIT(31) : 0);
	reg |= (cfg->pulse_mode_hsa_he ? BIT(28) : 0);
	reg |= (cfg->hfp_lp11_en ? BIT(24) : 0);
	reg |= (cfg->hbp_lp11_en ? BIT(20) : 0);
	reg |= (cfg->hsa_lp11_en ? BIT(16) : 0);
	reg |= (cfg->eof_bllp_lp11_en ? BIT(15) : 0);
	reg |= (cfg->bllp_lp11_en ? BIT(12) : 0);
	reg |= (cfg->traffic_mode & 0x3) << 8;
	reg |= (cfg->vc_id & 0x3);
	reg |= (video_mode_format_map[common_cfg->dst_format] & 0x3) << 4;
	DSI_W32(ctrl, DSI_VIDEO_MODE_CTRL, reg);

	reg = (common_cfg->swap_mode & 0x7) << 12;
	reg |= (common_cfg->bit_swap_red ? BIT(0) : 0);
	reg |= (common_cfg->bit_swap_green ? BIT(4) : 0);
	reg |= (common_cfg->bit_swap_blue ? BIT(8) : 0);
	DSI_W32(ctrl, DSI_VIDEO_MODE_DATA_CTRL, reg);
	/* Disable Timing double buffering */
	DSI_W32(ctrl, DSI_DSI_TIMING_DB_MODE, 0x0);

	DSI_CTRL_HW_DBG(ctrl, "Video engine setup done\n");
}

void dsi_ctrl_hw_cmn_debug_bus(struct dsi_ctrl_hw *ctrl, u32 *entries, u32 size)
{
	u32 reg = 0, i = 0;

	for (i = 0; i < size; i++) {
		DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, entries[i]);
		/* make sure that debug test point is enabled */
		wmb();
		reg = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);
		DSI_CTRL_HW_INFO(ctrl, "debug bus ctrl: 0x%x status:0x%x\n",
				entries[i], reg);
	}
}

/**
 * cmd_engine_setup() - setup dsi host controller for command mode
 * @ctrl:          Pointer to the controller host hardware.
 * @common_cfg:    Common configuration parameters.
 * @cfg:           Command mode configuration.
 *
 * Setup DSI CMD engine with a specific configuration. Controller and
 * command engine are not enabled as part of this function.
 */
void dsi_ctrl_hw_cmn_cmd_engine_setup(struct dsi_ctrl_hw *ctrl,
				     struct dsi_host_common_cfg *common_cfg,
				     struct dsi_cmd_engine_cfg *cfg)
{
	u32 reg = 0;

	reg = (cfg->max_cmd_packets_interleave & 0xF) << 20;
	reg |= (common_cfg->bit_swap_red ? BIT(4) : 0);
	reg |= (common_cfg->bit_swap_green ? BIT(8) : 0);
	reg |= (common_cfg->bit_swap_blue ? BIT(12) : 0);
	reg |= cmd_mode_format_map[common_cfg->dst_format];
	DSI_W32(ctrl, DSI_COMMAND_MODE_MDP_CTRL, reg);

	if (!cfg->mdp_idle_ctrl_en) {
		reg = DSI_R32(ctrl, DSI_COMMAND_MODE_MDP_CTRL2);
		reg |= BIT(16);
		DSI_W32(ctrl, DSI_COMMAND_MODE_MDP_CTRL2, reg);
	}

	reg = cfg->wr_mem_start & 0xFF;
	reg |= (cfg->wr_mem_continue & 0xFF) << 8;
	reg |= (cfg->insert_dcs_command ? BIT(16) : 0);
	DSI_W32(ctrl, DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL, reg);

	if (cfg->mdp_idle_ctrl_en) {
		reg = cfg->mdp_idle_ctrl_len & 0x3FF;
		reg |= BIT(12);
		DSI_W32(ctrl, DSI_COMMAND_MODE_MDP_IDLE_CTRL, reg);
	}

	DSI_CTRL_HW_DBG(ctrl, "Cmd engine setup done\n");
}

/**
 * video_engine_en() - enable DSI video engine
 * @ctrl:          Pointer to controller host hardware.
 * @on:            Enable/disabel video engine.
 */
void dsi_ctrl_hw_cmn_video_engine_en(struct dsi_ctrl_hw *ctrl, bool on)
{
	u32 reg = 0;

	/* Set/Clear VIDEO_MODE_EN bit */
	reg = DSI_R32(ctrl, DSI_CTRL);
	if (on)
		reg |= BIT(1);
	else
		reg &= ~BIT(1);

	DSI_W32(ctrl, DSI_CTRL, reg);

	DSI_CTRL_HW_DBG(ctrl, "Video engine = %d\n", on);
}

/**
 * ctrl_en() - enable DSI controller engine
 * @ctrl:          Pointer to the controller host hardware.
 * @on:            turn on/off the DSI controller engine.
 */
void dsi_ctrl_hw_cmn_ctrl_en(struct dsi_ctrl_hw *ctrl, bool on)
{
	u32 reg = 0;
	u32 clk_ctrl;

	clk_ctrl = DSI_R32(ctrl, DSI_CLK_CTRL);
	DSI_W32(ctrl, DSI_CLK_CTRL, clk_ctrl | DSI_CTRL_DYNAMIC_FORCE_ON);
	wmb(); /* wait for clocks to enable */

	/* Set/Clear DSI_EN bit */
	reg = DSI_R32(ctrl, DSI_CTRL);
	if (on)
		reg |= BIT(0);
	else
		reg &= ~BIT(0);

	DSI_W32(ctrl, DSI_CTRL, reg);
	wmb(); /* wait for DSI_EN update before disabling clocks */

	DSI_W32(ctrl, DSI_CLK_CTRL, clk_ctrl);
	wmb(); /* make sure clocks are restored */

	DSI_CTRL_HW_DBG(ctrl, "Controller engine = %d\n", on);
}

/**
 * cmd_engine_en() - enable DSI controller command engine
 * @ctrl:          Pointer to the controller host hardware.
 * @on:            Turn on/off the DSI command engine.
 */
void dsi_ctrl_hw_cmn_cmd_engine_en(struct dsi_ctrl_hw *ctrl, bool on)
{
	u32 reg = 0;

	/* Set/Clear CMD_MODE_EN bit */
	reg = DSI_R32(ctrl, DSI_CTRL);
	if (on)
		reg |= BIT(2);
	else
		reg &= ~BIT(2);

	DSI_W32(ctrl, DSI_CTRL, reg);

	DSI_CTRL_HW_DBG(ctrl, "command engine = %d\n", on);
}

/**
 * kickoff_command() - transmits commands stored in memory
 * @ctrl:          Pointer to the controller host hardware.
 * @cmd:           Command information.
 * @flags:         Modifiers for command transmission.
 *
 * The controller hardware is programmed with address and size of the
 * command buffer. The transmission is kicked off if
 * DSI_CTRL_HW_CMD_WAIT_FOR_TRIGGER flag is not set. If this flag is
 * set, caller should make a separate call to trigger_command_dma() to
 * transmit the command.
 */
/* A52_PHASE446L_HW_TRIGGER */
extern void a52_p446l_after_trigger(struct dsi_ctrl_hw *,u32);
void dsi_ctrl_hw_cmn_kickoff_command(struct dsi_ctrl_hw *ctrl,
				    struct dsi_ctrl_cmd_dma_info *cmd,
				    u32 flags)
{
	u32 reg = 0;

	/*Set BROADCAST_EN and EMBEDDED_MODE */
	reg = DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL);
	if (cmd->en_broadcast)
		reg |= BIT(31);
	else
		reg &= ~BIT(31);

	if (cmd->is_master)
		reg |= BIT(30);
	else
		reg &= ~BIT(30);

	if (cmd->use_lpm)
		reg |= BIT(26);
	else
		reg &= ~BIT(26);

	reg |= BIT(28);/* Select embedded mode */
	reg &= ~BIT(24);/* packet type */
	reg &= ~BIT(29);/* WC_SEL to 0 */
	DSI_W32(ctrl, DSI_COMMAND_MODE_DMA_CTRL, reg);

	reg = DSI_R32(ctrl, DSI_DMA_FIFO_CTRL);
	reg |= BIT(20);/* Disable write watermark*/
	reg |= BIT(16);/* Disable read watermark */

	DSI_W32(ctrl, DSI_DMA_FIFO_CTRL, reg);
	DSI_W32(ctrl, DSI_DMA_CMD_OFFSET, cmd->offset);
	DSI_W32(ctrl, DSI_DMA_CMD_LENGTH, (cmd->length & 0xFFFFFF));

	/* wait for writes to complete before kick off */
	wmb();

	if (a52_p293_gdm_trace_active()) {
		a52_p409_hot_record(ctrl, A52_P409_DMA_PROGRAM, 0, 0);
		a52_ackfr_record("P276 303 S05 dc=%x off=%x len=%x fc=%x",
			DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL),
			DSI_R32(ctrl, DSI_DMA_CMD_OFFSET),
			DSI_R32(ctrl, DSI_DMA_CMD_LENGTH),
			DSI_R32(ctrl, DSI_DMA_FIFO_CTRL));
		a52_ackfr_record("P276 303 S05b tr=%x sw=%x cc=%x",
			DSI_R32(ctrl, DSI_TRIG_CTRL),
			DSI_R32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER),
			DSI_R32(ctrl, DSI_CLK_CTRL));
	}

	if (!(flags & DSI_CTRL_HW_CMD_WAIT_FOR_TRIGGER)) {
		if (a52_p421_target_active()) {
			a52_p421_survival_record(7U, 0U, 0U, 0, 0U, 0);
			a52_p424_snapshot(ctrl);
			a52_p427_road_snapshot(ctrl, 0U);
			a52_p430_dma_arm(ctrl);
		}
		if (a52_p439_f0_active())
			a52_p439_begin(ctrl);
		if (a52_p445_active()) a52_p445_pre_trigger(ctrl);
	DSI_W32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER, 0x1);
		a52_p446l_after_trigger(ctrl,flags);
	if (a52_p445_active()) a52_p445_post_trigger(ctrl);
		if (a52_p439_f0_active())
			a52_p439_post_trigger(ctrl);
		if (a52_p421_target_active()) {
			a52_p430_dma_triggered(ctrl);
			a52_p421_survival_record(8U, 0U, 0U, 0, 0U, 0);
		}
		if (a52_p293_gdm_trace_active()) {
			a52_ackfr_record("P276 303 S06 st=%x fs=%x ln=%x ck=%x",
				DSI_R32(ctrl, DSI_STATUS), DSI_R32(ctrl, DSI_FIFO_STATUS),
				DSI_R32(ctrl, DSI_LANE_STATUS), DSI_R32(ctrl, DSI_CLK_STATUS));
			a52_ackfr_record("P276 303 S06b tg=%x in=%x",
				DSI_R32(ctrl, DSI_TPG_DMA_FIFO_STATUS), DSI_R32(ctrl, DSI_INT_CTRL));
		}
	}
}

/**
 * kickoff_fifo_command() - transmits a command using FIFO in dsi
 *                          hardware.
 * @ctrl:          Pointer to the controller host hardware.
 * @cmd:           Command information.
 * @flags:         Modifiers for command transmission.
 *
 * The controller hardware FIFO is programmed with command header and
 * payload. The transmission is kicked off if
 * DSI_CTRL_HW_CMD_WAIT_FOR_TRIGGER flag is not set. If this flag is
 * set, caller should make a separate call to trigger_command_dma() to
 * transmit the command.
 */
void dsi_ctrl_hw_cmn_kickoff_fifo_command(struct dsi_ctrl_hw *ctrl,
					 struct dsi_ctrl_cmd_dma_fifo_info *cmd,
					 u32 flags)
{
	u32 reg = 0, i = 0;
	u32 *ptr = cmd->command;
	/*
	 * Set CMD_DMA_TPG_EN, TPG_DMA_FIFO_MODE and
	 * CMD_DMA_PATTERN_SEL = custom pattern stored in TPG DMA FIFO
	 */
	reg = (BIT(1) | BIT(2) | (0x3 << 16));
	DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CTRL, reg);

	/*
	 * Program the FIFO with command buffer. Hardware requires an extra
	 * DWORD (set to zero) if the length of command buffer is odd DWORDS.
	 */
	for (i = 0; i < cmd->size; i += 4) {
		DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, *ptr);
		ptr++;
	}

	if ((cmd->size / 4) & 0x1)
		DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0);

	/*Set BROADCAST_EN and EMBEDDED_MODE */
	reg = DSI_R32(ctrl, DSI_COMMAND_MODE_DMA_CTRL);
	if (cmd->en_broadcast)
		reg |= BIT(31);
	else
		reg &= ~BIT(31);

	if (cmd->is_master)
		reg |= BIT(30);
	else
		reg &= ~BIT(30);

	if (cmd->use_lpm)
		reg |= BIT(26);
	else
		reg &= ~BIT(26);

	reg |= BIT(28);

	DSI_W32(ctrl, DSI_COMMAND_MODE_DMA_CTRL, reg);

	DSI_W32(ctrl, DSI_DMA_CMD_LENGTH, (cmd->size & 0xFFFFFFFF));
	/* Finish writes before command trigger */
	wmb();

	if (!(flags & DSI_CTRL_HW_CMD_WAIT_FOR_TRIGGER)) {
		DSI_W32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER, 0x1);
		a52_p446l_after_trigger(ctrl,flags);
	}

	DSI_CTRL_HW_DBG(ctrl, "size=%d, trigger = %d\n", cmd->size,
		 (flags & DSI_CTRL_HW_CMD_WAIT_FOR_TRIGGER) ? false : true);
}

void dsi_ctrl_hw_cmn_reset_cmd_fifo(struct dsi_ctrl_hw *ctrl)
{
	/* disable cmd dma tpg */
	DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CTRL, 0x0);

	DSI_W32(ctrl, DSI_TPG_DMA_FIFO_RESET, 0x1);
	udelay(1);
	DSI_W32(ctrl, DSI_TPG_DMA_FIFO_RESET, 0x0);
}

/**
 * trigger_command_dma() - trigger transmission of command buffer.
 * @ctrl:          Pointer to the controller host hardware.
 *
 * This trigger can be only used if there was a prior call to
 * kickoff_command() of kickoff_fifo_command() with
 * DSI_CTRL_HW_CMD_WAIT_FOR_TRIGGER flag.
 */
void dsi_ctrl_hw_cmn_trigger_command_dma(struct dsi_ctrl_hw *ctrl)
{
	if (a52_p439_f0_active())
		a52_p439_begin(ctrl);
	if (a52_p445_active()) a52_p445_pre_trigger(ctrl);
	DSI_W32(ctrl, DSI_CMD_MODE_DMA_SW_TRIGGER, 0x1);
	a52_p446l_after_trigger(ctrl,0x80000000U);
	if (a52_p445_active()) a52_p445_post_trigger(ctrl);
	if (a52_p439_f0_active())
		a52_p439_post_trigger(ctrl);
	if (a52_p293_gdm_trace_active()) {
		a52_ackfr_record("P276 303 S06 st=%x fs=%x ln=%x ck=%x",
			DSI_R32(ctrl, DSI_STATUS), DSI_R32(ctrl, DSI_FIFO_STATUS),
			DSI_R32(ctrl, DSI_LANE_STATUS), DSI_R32(ctrl, DSI_CLK_STATUS));
		a52_ackfr_record("P276 303 S06b tg=%x in=%x",
			DSI_R32(ctrl, DSI_TPG_DMA_FIFO_STATUS), DSI_R32(ctrl, DSI_INT_CTRL));
	}
}

/**
 * clear_rdbk_reg() - clear previously read panel data.
 * @ctrl:          Pointer to the controller host hardware.
 *
 * This function is called before sending DSI Rx command to
 * panel in order to clear if any stale data remaining from
 * previous read operation.
 */
void dsi_ctrl_hw_cmn_clear_rdbk_reg(struct dsi_ctrl_hw *ctrl)
{
	DSI_W32(ctrl, DSI_RDBK_DATA_CTRL, 0x1);
	wmb(); /* ensure read back register is reset */
	DSI_W32(ctrl, DSI_RDBK_DATA_CTRL, 0x0);
	wmb(); /* ensure read back register is cleared */
}

/**
 * get_cmd_read_data() - get data read from the peripheral
 * @ctrl:           Pointer to the controller host hardware.
 * @rd_buf:         Buffer where data will be read into.
 * @total_read_len: Number of bytes to read.
 *
 * return: number of bytes read.
 */
u32 dsi_ctrl_hw_cmn_get_cmd_read_data(struct dsi_ctrl_hw *ctrl,
				     u8 *rd_buf,
				     u32 read_offset,
				     u32 rx_byte,
				     u32 pkt_size,
				     u32 *hw_read_cnt)
{
	u32 *lp, *temp, data;
	int i, j = 0, cnt, off;
	u32 read_cnt;
	u32 repeated_bytes = 0;
	u8 reg[16] = {0};
	bool ack_err = false;

	lp = (u32 *)rd_buf;
	temp = (u32 *)reg;
	cnt = (rx_byte + 3) >> 2;

	if (cnt > 4)
		cnt = 4;

	read_cnt = (DSI_R32(ctrl, DSI_RDBK_DATA_CTRL) >> 16);
	ack_err = (rx_byte == 4) ? (read_cnt == 8) :
			((read_cnt - 4) == (pkt_size + 6));

	if (ack_err)
		read_cnt -= 4;
	if (!read_cnt) {
		DSI_CTRL_HW_ERR(ctrl, "Panel detected error, no data read\n");
		return 0;
	}

	if (read_cnt > 16) {
		int bytes_shifted, data_lost = 0, rem_header = 0;

		bytes_shifted = read_cnt - rx_byte;
		if (bytes_shifted >= 4)
			data_lost = bytes_shifted - 4; /* remove DCS header */
		else
			rem_header = 4 - bytes_shifted; /* remaining header */

		repeated_bytes = (read_offset - 4) - data_lost + rem_header;
	}

	off = DSI_RDBK_DATA0;
	off += ((cnt - 1) * 4);

	for (i = 0; i < cnt; i++) {
		data = DSI_R32(ctrl, off);
		if (!repeated_bytes)
			*lp++ = ntohl(data);
		else
			*temp++ = ntohl(data);
		off -= 4;
	}

	if (repeated_bytes) {
		for (i = repeated_bytes; i < 16; i++)
			rd_buf[j++] = reg[i];
	}

	*hw_read_cnt = read_cnt;
	DSI_CTRL_HW_DBG(ctrl, "Read %d bytes\n", rx_byte);
	return rx_byte;
}

/**
 * get_interrupt_status() - returns the interrupt status
 * @ctrl:          Pointer to the controller host hardware.
 *
 * Returns the ORed list of interrupts(enum dsi_status_int_type) that
 * are active. This list does not include any error interrupts. Caller
 * should call get_error_status for error interrupts.
 *
 * Return: List of active interrupts.
 */
u32 dsi_ctrl_hw_cmn_get_interrupt_status(struct dsi_ctrl_hw *ctrl)
{
	u32 reg = 0;
	u32 ints = 0;

	reg = DSI_R32(ctrl, DSI_INT_CTRL);

	if (reg & BIT(0))
		ints |= DSI_CMD_MODE_DMA_DONE;
	if (reg & BIT(8))
		ints |= DSI_CMD_FRAME_DONE;
	if (reg & BIT(10))
		ints |= DSI_CMD_STREAM0_FRAME_DONE;
	if (reg & BIT(12))
		ints |= DSI_CMD_STREAM1_FRAME_DONE;
	if (reg & BIT(14))
		ints |= DSI_CMD_STREAM2_FRAME_DONE;
	if (reg & BIT(16))
		ints |= DSI_VIDEO_MODE_FRAME_DONE;
	if (reg & BIT(20))
		ints |= DSI_BTA_DONE;
	if (reg & BIT(28))
		ints |= DSI_DYN_REFRESH_DONE;
	if (reg & BIT(30))
		ints |= DSI_DESKEW_DONE;
	if (reg & BIT(24))
		ints |= DSI_ERROR;

	DSI_CTRL_HW_DBG(ctrl, "Interrupt status = 0x%x, INT_CTRL=0x%x\n",
		 ints, reg);
	return ints;
}

/**
 * clear_interrupt_status() - clears the specified interrupts
 * @ctrl:          Pointer to the controller host hardware.
 * @ints:          List of interrupts to be cleared.
 */
void dsi_ctrl_hw_cmn_clear_interrupt_status(struct dsi_ctrl_hw *ctrl, u32 ints)
{
	u32 reg = 0;

	reg = DSI_R32(ctrl, DSI_INT_CTRL);

	if (ints & DSI_CMD_MODE_DMA_DONE)
		reg |= BIT(0);
	if (ints & DSI_CMD_FRAME_DONE)
		reg |= BIT(8);
	if (ints & DSI_CMD_STREAM0_FRAME_DONE)
		reg |= BIT(10);
	if (ints & DSI_CMD_STREAM1_FRAME_DONE)
		reg |= BIT(12);
	if (ints & DSI_CMD_STREAM2_FRAME_DONE)
		reg |= BIT(14);
	if (ints & DSI_VIDEO_MODE_FRAME_DONE)
		reg |= BIT(16);
	if (ints & DSI_BTA_DONE)
		reg |= BIT(20);
	if (ints & DSI_DYN_REFRESH_DONE)
		reg |= BIT(28);
	if (ints & DSI_DESKEW_DONE)
		reg |= BIT(30);

	/*
	 * Do not clear error status.
	 * It will be cleared as part of
	 * error handler function.
	 */
	reg &= ~BIT(24);
	DSI_W32(ctrl, DSI_INT_CTRL, reg);

	DSI_CTRL_HW_DBG(ctrl, "Clear interrupts, ints = 0x%x, INT_CTRL=0x%x\n",
		 ints, reg);
}

/**
 * enable_status_interrupts() - enable the specified interrupts
 * @ctrl:          Pointer to the controller host hardware.
 * @ints:          List of interrupts to be enabled.
 *
 * Enables the specified interrupts. This list will override the
 * previous interrupts enabled through this function. Caller has to
 * maintain the state of the interrupts enabled. To disable all
 * interrupts, set ints to 0.
 */
void dsi_ctrl_hw_cmn_enable_status_interrupts(
		struct dsi_ctrl_hw *ctrl, u32 ints)
{
	u32 reg = 0;

	/* Do not change value of DSI_ERROR_MASK bit */
	reg |= (DSI_R32(ctrl, DSI_INT_CTRL) & BIT(25));
	if (ints & DSI_CMD_MODE_DMA_DONE)
		reg |= BIT(1);
	if (ints & DSI_CMD_FRAME_DONE)
		reg |= BIT(9);
	if (ints & DSI_CMD_STREAM0_FRAME_DONE)
		reg |= BIT(11);
	if (ints & DSI_CMD_STREAM1_FRAME_DONE)
		reg |= BIT(13);
	if (ints & DSI_CMD_STREAM2_FRAME_DONE)
		reg |= BIT(15);
	if (ints & DSI_VIDEO_MODE_FRAME_DONE)
		reg |= BIT(17);
	if (ints & DSI_BTA_DONE)
		reg |= BIT(21);
	if (ints & DSI_DYN_REFRESH_DONE)
		reg |= BIT(29);
	if (ints & DSI_DESKEW_DONE)
		reg |= BIT(31);

	DSI_W32(ctrl, DSI_INT_CTRL, reg);

	DSI_CTRL_HW_DBG(ctrl, "Enable interrupts 0x%x, INT_CTRL=0x%x\n", ints,
			reg);
}

/**
 * get_error_status() - returns the error status
 * @ctrl:          Pointer to the controller host hardware.
 *
 * Returns the ORed list of errors(enum dsi_error_int_type) that are
 * active. This list does not include any status interrupts. Caller
 * should call get_interrupt_status for status interrupts.
 *
 * Return: List of active error interrupts.
 */
u64 dsi_ctrl_hw_cmn_get_error_status(struct dsi_ctrl_hw *ctrl)
{
	u32 dln0_phy_err;
	u32 fifo_status;
	u32 ack_error;
	u32 timeout_errors;
	u32 clk_error;
	u32 dsi_status;
	u64 errors = 0, shift = 0x1;

	dln0_phy_err = DSI_R32(ctrl, DSI_DLN0_PHY_ERR);
	if (dln0_phy_err & BIT(0))
		errors |= DSI_DLN0_ESC_ENTRY_ERR;
	if (dln0_phy_err & BIT(4))
		errors |= DSI_DLN0_ESC_SYNC_ERR;
	if (dln0_phy_err & BIT(8))
		errors |= DSI_DLN0_LP_CONTROL_ERR;
	if (dln0_phy_err & BIT(12))
		errors |= DSI_DLN0_LP0_CONTENTION;
	if (dln0_phy_err & BIT(16))
		errors |= DSI_DLN0_LP1_CONTENTION;

	fifo_status = DSI_R32(ctrl, DSI_FIFO_STATUS);
	if (fifo_status & BIT(7))
		errors |= DSI_CMD_MDP_FIFO_UNDERFLOW;
	if (fifo_status & BIT(10))
		errors |= DSI_CMD_DMA_FIFO_UNDERFLOW;
	if (fifo_status & BIT(18))
		errors |= DSI_DLN0_HS_FIFO_OVERFLOW;
	if (fifo_status & BIT(19))
		errors |= DSI_DLN0_HS_FIFO_UNDERFLOW;
	if (fifo_status & BIT(22))
		errors |= DSI_DLN1_HS_FIFO_OVERFLOW;
	if (fifo_status & BIT(23))
		errors |= DSI_DLN1_HS_FIFO_UNDERFLOW;
	if (fifo_status & BIT(26))
		errors |= DSI_DLN2_HS_FIFO_OVERFLOW;
	if (fifo_status & BIT(27))
		errors |= DSI_DLN2_HS_FIFO_UNDERFLOW;
	if (fifo_status & BIT(30))
		errors |= DSI_DLN3_HS_FIFO_OVERFLOW;
	if (fifo_status & BIT(31))
		errors |= DSI_DLN3_HS_FIFO_UNDERFLOW;

	ack_error = DSI_R32(ctrl, DSI_ACK_ERR_STATUS);
	if (ack_error & BIT(16))
		errors |= DSI_RDBK_SINGLE_ECC_ERR;
	if (ack_error & BIT(17))
		errors |= DSI_RDBK_MULTI_ECC_ERR;
	if (ack_error & BIT(20))
		errors |= DSI_RDBK_CRC_ERR;
	if (ack_error & BIT(23))
		errors |= DSI_RDBK_INCOMPLETE_PKT;
	if (ack_error & BIT(24))
		errors |= DSI_PERIPH_ERROR_PKT;
	if (ack_error & BIT(15))
		errors |= (shift << DSI_EINT_PANEL_SPECIFIC_ERR);

	timeout_errors = DSI_R32(ctrl, DSI_TIMEOUT_STATUS);
	if (timeout_errors & BIT(0))
		errors |= DSI_HS_TX_TIMEOUT;
	if (timeout_errors & BIT(4))
		errors |= DSI_LP_RX_TIMEOUT;
	if (timeout_errors & BIT(8))
		errors |= DSI_BTA_TIMEOUT;

	clk_error = DSI_R32(ctrl, DSI_CLK_STATUS);
	if (clk_error & BIT(16))
		errors |= DSI_PLL_UNLOCK;

	dsi_status = DSI_R32(ctrl, DSI_STATUS);
	if (dsi_status & BIT(31))
		errors |= DSI_INTERLEAVE_OP_CONTENTION;

	DSI_CTRL_HW_DBG(ctrl, "Error status = 0x%llx, phy=0x%x, fifo=0x%x\n",
		 errors, dln0_phy_err, fifo_status);
	DSI_CTRL_HW_DBG(ctrl, "ack=0x%x, timeout=0x%x, clk=0x%x, dsi=0x%x\n",
		 ack_error, timeout_errors, clk_error, dsi_status);
	return errors;
}

/**
 * clear_error_status() - clears the specified errors
 * @ctrl:          Pointer to the controller host hardware.
 * @errors:          List of errors to be cleared.
 */
void dsi_ctrl_hw_cmn_clear_error_status(struct dsi_ctrl_hw *ctrl, u64 errors)
{
	u32 dln0_phy_err = 0;
	u32 fifo_status = 0;
	u32 ack_error = 0;
	u32 timeout_error = 0;
	u32 clk_error = 0;
	u32 dsi_status = 0;

	if (errors & DSI_RDBK_SINGLE_ECC_ERR)
		ack_error |= BIT(16);
	if (errors & DSI_RDBK_MULTI_ECC_ERR)
		ack_error |= BIT(17);
	if (errors & DSI_RDBK_CRC_ERR)
		ack_error |= BIT(20);
	if (errors & DSI_RDBK_INCOMPLETE_PKT)
		ack_error |= BIT(23);
	if (errors & DSI_PERIPH_ERROR_PKT)
		ack_error |= BIT(24);
	if (errors & DSI_PANEL_SPECIFIC_ERR)
		ack_error |= BIT(15);

	if (errors & DSI_LP_RX_TIMEOUT)
		timeout_error |= BIT(4);
	if (errors & DSI_HS_TX_TIMEOUT)
		timeout_error |= BIT(0);
	if (errors & DSI_BTA_TIMEOUT)
		timeout_error |= BIT(8);

	if (errors & DSI_PLL_UNLOCK)
		clk_error |= BIT(16);

	if (errors & DSI_DLN0_LP0_CONTENTION)
		dln0_phy_err |= BIT(12);
	if (errors & DSI_DLN0_LP1_CONTENTION)
		dln0_phy_err |= BIT(16);
	if (errors & DSI_DLN0_ESC_ENTRY_ERR)
		dln0_phy_err |= BIT(0);
	if (errors & DSI_DLN0_ESC_SYNC_ERR)
		dln0_phy_err |= BIT(4);
	if (errors & DSI_DLN0_LP_CONTROL_ERR)
		dln0_phy_err |= BIT(8);

	if (errors & DSI_CMD_DMA_FIFO_UNDERFLOW)
		fifo_status |= BIT(10);
	if (errors & DSI_CMD_MDP_FIFO_UNDERFLOW)
		fifo_status |= BIT(7);
	if (errors & DSI_DLN0_HS_FIFO_OVERFLOW)
		fifo_status |= BIT(18);
	if (errors & DSI_DLN1_HS_FIFO_OVERFLOW)
		fifo_status |= BIT(22);
	if (errors & DSI_DLN2_HS_FIFO_OVERFLOW)
		fifo_status |= BIT(26);
	if (errors & DSI_DLN3_HS_FIFO_OVERFLOW)
		fifo_status |= BIT(30);
	if (errors & DSI_DLN0_HS_FIFO_UNDERFLOW)
		fifo_status |= BIT(19);
	if (errors & DSI_DLN1_HS_FIFO_UNDERFLOW)
		fifo_status |= BIT(23);
	if (errors & DSI_DLN2_HS_FIFO_UNDERFLOW)
		fifo_status |= BIT(27);
	if (errors & DSI_DLN3_HS_FIFO_UNDERFLOW)
		fifo_status |= BIT(31);

	if (errors & DSI_INTERLEAVE_OP_CONTENTION)
		dsi_status |= BIT(31);

	DSI_W32(ctrl, DSI_DLN0_PHY_ERR, dln0_phy_err);
	DSI_W32(ctrl, DSI_FIFO_STATUS, fifo_status);
	/* Writing of an extra 0 is needed to clear ack error bits */
	DSI_W32(ctrl, DSI_ACK_ERR_STATUS, ack_error);
	wmb(); /* make sure register is committed */
	DSI_W32(ctrl, DSI_ACK_ERR_STATUS, 0x0);
	DSI_W32(ctrl, DSI_TIMEOUT_STATUS, timeout_error);
	DSI_W32(ctrl, DSI_CLK_STATUS, clk_error);
	DSI_W32(ctrl, DSI_STATUS, dsi_status);

	DSI_CTRL_HW_DBG(ctrl, "clear errors = 0x%llx, phy=0x%x, fifo=0x%x\n",
		 errors, dln0_phy_err, fifo_status);
	DSI_CTRL_HW_DBG(ctrl, "ack=0x%x, timeout=0x%x, clk=0x%x, dsi=0x%x\n",
		 ack_error, timeout_error, clk_error, dsi_status);
}

/**
 * enable_error_interrupts() - enable the specified interrupts
 * @ctrl:          Pointer to the controller host hardware.
 * @errors:        List of errors to be enabled.
 *
 * Enables the specified interrupts. This list will override the
 * previous interrupts enabled through this function. Caller has to
 * maintain the state of the interrupts enabled. To disable all
 * interrupts, set errors to 0.
 */
void dsi_ctrl_hw_cmn_enable_error_interrupts(struct dsi_ctrl_hw *ctrl,
					    u64 errors)
{
	u32 int_ctrl = 0;
	u32 int_mask0 = 0x7FFF3BFF;

	int_ctrl = DSI_R32(ctrl, DSI_INT_CTRL);
	if (errors)
		int_ctrl |= BIT(25);
	else
		int_ctrl &= ~BIT(25);

	if (errors & DSI_RDBK_SINGLE_ECC_ERR)
		int_mask0 &= ~BIT(0);
	if (errors & DSI_RDBK_MULTI_ECC_ERR)
		int_mask0 &= ~BIT(1);
	if (errors & DSI_RDBK_CRC_ERR)
		int_mask0 &= ~BIT(2);
	if (errors & DSI_RDBK_INCOMPLETE_PKT)
		int_mask0 &= ~BIT(3);
	if (errors & DSI_PERIPH_ERROR_PKT)
		int_mask0 &= ~BIT(4);

	if (errors & DSI_LP_RX_TIMEOUT)
		int_mask0 &= ~BIT(5);
	if (errors & DSI_HS_TX_TIMEOUT)
		int_mask0 &= ~BIT(6);
	if (errors & DSI_BTA_TIMEOUT)
		int_mask0 &= ~BIT(7);

	if (errors & DSI_PLL_UNLOCK)
		int_mask0 &= ~BIT(28);

	if (errors & DSI_DLN0_LP0_CONTENTION)
		int_mask0 &= ~BIT(24);
	if (errors & DSI_DLN0_LP1_CONTENTION)
		int_mask0 &= ~BIT(25);
	if (errors & DSI_DLN0_ESC_ENTRY_ERR)
		int_mask0 &= ~BIT(21);
	if (errors & DSI_DLN0_ESC_SYNC_ERR)
		int_mask0 &= ~BIT(22);
	if (errors & DSI_DLN0_LP_CONTROL_ERR)
		int_mask0 &= ~BIT(23);

	if (errors & DSI_CMD_DMA_FIFO_UNDERFLOW)
		int_mask0 &= ~BIT(9);
	if (errors & DSI_CMD_MDP_FIFO_UNDERFLOW)
		int_mask0 &= ~BIT(11);
	if (errors & DSI_DLN0_HS_FIFO_OVERFLOW)
		int_mask0 &= ~BIT(16);
	if (errors & DSI_DLN1_HS_FIFO_OVERFLOW)
		int_mask0 &= ~BIT(17);
	if (errors & DSI_DLN2_HS_FIFO_OVERFLOW)
		int_mask0 &= ~BIT(18);
	if (errors & DSI_DLN3_HS_FIFO_OVERFLOW)
		int_mask0 &= ~BIT(19);
	if (errors & DSI_DLN0_HS_FIFO_UNDERFLOW)
		int_mask0 &= ~BIT(26);
	if (errors & DSI_DLN1_HS_FIFO_UNDERFLOW)
		int_mask0 &= ~BIT(27);
	if (errors & DSI_DLN2_HS_FIFO_UNDERFLOW)
		int_mask0 &= ~BIT(29);
	if (errors & DSI_DLN3_HS_FIFO_UNDERFLOW)
		int_mask0 &= ~BIT(30);

	if (errors & DSI_INTERLEAVE_OP_CONTENTION)
		int_mask0 &= ~BIT(8);

	DSI_W32(ctrl, DSI_INT_CTRL, int_ctrl);
	DSI_W32(ctrl, DSI_ERR_INT_MASK0, int_mask0);

	DSI_CTRL_HW_DBG(ctrl, "[DSI_%d] enable errors = 0x%llx, int_mask0=0x%x\n",
		 errors, int_mask0);
}

/**
 * video_test_pattern_setup() - setup test pattern engine for video mode
 * @ctrl:          Pointer to the controller host hardware.
 * @type:          Type of test pattern.
 * @init_val:      Initial value to use for generating test pattern.
 */
void dsi_ctrl_hw_cmn_video_test_pattern_setup(struct dsi_ctrl_hw *ctrl,
					     enum dsi_test_pattern type,
					     u32 init_val)
{
	u32 reg = 0;

	DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_VIDEO_INIT_VAL, init_val);

	switch (type) {
	case DSI_TEST_PATTERN_FIXED:
		reg |= (0x2 << 4);
		break;
	case DSI_TEST_PATTERN_INC:
		reg |= (0x1 << 4);
		break;
	case DSI_TEST_PATTERN_POLY:
		DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_VIDEO_POLY, 0xF0F0F);
		break;
	default:
		break;
	}

	DSI_W32(ctrl, DSI_TPG_MAIN_CONTROL, 0x100);
	DSI_W32(ctrl, DSI_TPG_VIDEO_CONFIG, 0x5);
	DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CTRL, reg);

	DSI_CTRL_HW_DBG(ctrl, "Video test pattern setup done\n");
}

/**
 * cmd_test_pattern_setup() - setup test patttern engine for cmd mode
 * @ctrl:          Pointer to the controller host hardware.
 * @type:          Type of test pattern.
 * @init_val:      Initial value to use for generating test pattern.
 * @stream_id:     Stream Id on which packets are generated.
 */
void dsi_ctrl_hw_cmn_cmd_test_pattern_setup(struct dsi_ctrl_hw *ctrl,
					   enum dsi_test_pattern type,
					   u32 init_val,
					   u32 stream_id)
{
	u32 reg = 0;
	u32 init_offset;
	u32 poly_offset;
	u32 pattern_sel_shift;

	switch (stream_id) {
	case 0:
		init_offset = DSI_TEST_PATTERN_GEN_CMD_MDP_INIT_VAL0;
		poly_offset = DSI_TEST_PATTERN_GEN_CMD_MDP_STREAM0_POLY;
		pattern_sel_shift = 8;
		break;
	case 1:
		init_offset = DSI_TEST_PATTERN_GEN_CMD_MDP_INIT_VAL1;
		poly_offset = DSI_TEST_PATTERN_GEN_CMD_MDP_STREAM1_POLY;
		pattern_sel_shift = 12;
		break;
	case 2:
		init_offset = DSI_TEST_PATTERN_GEN_CMD_MDP_INIT_VAL2;
		poly_offset = DSI_TEST_PATTERN_GEN_CMD_MDP_STREAM2_POLY;
		pattern_sel_shift = 20;
		break;
	default:
		return;
	}

	DSI_W32(ctrl, init_offset, init_val);

	switch (type) {
	case DSI_TEST_PATTERN_FIXED:
		reg |= (0x2 << pattern_sel_shift);
		break;
	case DSI_TEST_PATTERN_INC:
		reg |= (0x1 << pattern_sel_shift);
		break;
	case DSI_TEST_PATTERN_POLY:
		DSI_W32(ctrl, poly_offset, 0xF0F0F);
		break;
	default:
		break;
	}

	DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CTRL, reg);
	DSI_CTRL_HW_DBG(ctrl, "Cmd test pattern setup done\n");
}

/**
 * test_pattern_enable() - enable test pattern engine
 * @ctrl:          Pointer to the controller host hardware.
 * @enable:        Enable/Disable test pattern engine.
 */
void dsi_ctrl_hw_cmn_test_pattern_enable(struct dsi_ctrl_hw *ctrl,
					bool enable)
{
	u32 reg = DSI_R32(ctrl, DSI_TEST_PATTERN_GEN_CTRL);

	if (enable)
		reg |= BIT(0);
	else
		reg &= ~BIT(0);

	DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CTRL, reg);

	DSI_CTRL_HW_DBG(ctrl, "Test pattern enable=%d\n", enable);
}

/**
 * trigger_cmd_test_pattern() - trigger a command mode frame update with
 *                              test pattern
 * @ctrl:          Pointer to the controller host hardware.
 * @stream_id:     Stream on which frame update is sent.
 */
void dsi_ctrl_hw_cmn_trigger_cmd_test_pattern(struct dsi_ctrl_hw *ctrl,
					     u32 stream_id)
{
	switch (stream_id) {
	case 0:
		DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CMD_STREAM0_TRIGGER, 0x1);
		break;
	case 1:
		DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CMD_STREAM1_TRIGGER, 0x1);
		break;
	case 2:
		DSI_W32(ctrl, DSI_TEST_PATTERN_GEN_CMD_STREAM2_TRIGGER, 0x1);
		break;
	default:
		break;
	}

	DSI_CTRL_HW_DBG(ctrl, "Cmd Test pattern trigger\n");
}

void dsi_ctrl_hw_dln0_phy_err(struct dsi_ctrl_hw *ctrl)
{
	u32 status = 0;
	/*
	 * Clear out any phy errors prior to exiting ULPS
	 * This fixes certain instances where phy does not exit
	 * ULPS cleanly. Also, do not print error during such cases.
	 */
	status = DSI_R32(ctrl, DSI_DLN0_PHY_ERR);
	if (status & 0x011111) {
		DSI_W32(ctrl, DSI_DLN0_PHY_ERR, status);
		DSI_CTRL_HW_ERR(ctrl, "phy_err_status = %x\n", status);
	}
}

void dsi_ctrl_hw_cmn_phy_reset_config(struct dsi_ctrl_hw *ctrl,
		bool enable)
{
	u32 reg = 0;

	reg = DSI_MMSS_MISC_R32(ctrl, MMSS_MISC_CLAMP_REG_OFF);

	/* Mask/unmask disable PHY reset bit */
	if (enable)
		reg |= BIT(30);
	else
		reg &= ~BIT(30);

	DSI_MMSS_MISC_W32(ctrl, MMSS_MISC_CLAMP_REG_OFF, reg);
}

int dsi_ctrl_hw_cmn_ctrl_reset(struct dsi_ctrl_hw *ctrl,
		int mask)
{
	int rc = 0;
	u32 data;

	DSI_CTRL_HW_DBG(ctrl, "DSI CTRL and PHY reset, mask=%d\n", mask);

	data = DSI_R32(ctrl, 0x0004);
	/* Disable DSI video mode */
	DSI_W32(ctrl, 0x004, (data & ~BIT(1)));
	wmb(); /* ensure register committed */
	/* Disable DSI controller */
	DSI_W32(ctrl, 0x004, (data & ~(BIT(0) | BIT(1))));
	wmb(); /* ensure register committed */
	/* "Force On" all dynamic clocks */
	DSI_W32(ctrl, 0x11c, 0x100a00);

	/* DSI_SW_RESET */
	DSI_W32(ctrl, 0x118, 0x1);
	wmb(); /* ensure register is committed */
	DSI_W32(ctrl, 0x118, 0x0);
	wmb(); /* ensure register is committed */

	/* Remove "Force On" all dynamic clocks */
	DSI_W32(ctrl, 0x11c, 0x00);
	/* Enable DSI controller */
	DSI_W32(ctrl, 0x004, (data & ~BIT(1)));
	wmb(); /* ensure register committed */

	return rc;
}

void dsi_ctrl_hw_cmn_mask_error_intr(struct dsi_ctrl_hw *ctrl, u32 idx, bool en)
{
	u32 reg = 0;
	u32 fifo_status = 0, timeout_status = 0;
	u32 overflow_clear = BIT(10) | BIT(18) | BIT(22) | BIT(26) | BIT(30);
	u32 underflow_clear = BIT(19) | BIT(23) | BIT(27) | BIT(31);
	u32 lp_rx_clear = BIT(4);

	reg = DSI_R32(ctrl, 0x10c);

	/*
	 * Before unmasking we should clear the corresponding error status bits
	 * that might have been set while we masked these errors. Since these
	 * are sticky bits, these errors will trigger the moment we unmask
	 * the error bits.
	 */
	if (idx & BIT(DSI_FIFO_OVERFLOW)) {
		if (en) {
			reg |= (0x1f << 16);
			reg |= BIT(9);
		} else {
			reg &= ~(0x1f << 16);
			reg &= ~BIT(9);
			fifo_status = DSI_R32(ctrl, 0x00c);
			DSI_W32(ctrl, 0x00c, fifo_status | overflow_clear);
		}
	}

	if (idx & BIT(DSI_FIFO_UNDERFLOW)) {
		if (en)
			reg |= (0x1b << 26);
		else {
			reg &= ~(0x1b << 26);
			fifo_status = DSI_R32(ctrl, 0x00c);
			DSI_W32(ctrl, 0x00c, fifo_status | underflow_clear);
		}
	}

	if (idx & BIT(DSI_LP_Rx_TIMEOUT)) {
		if (en)
			reg |= (0x7 << 23);
		else {
			reg &= ~(0x7 << 23);
			timeout_status = DSI_R32(ctrl, 0x0c0);
			DSI_W32(ctrl, 0x0c0, timeout_status | lp_rx_clear);
		}
	}

	if (idx & BIT(DSI_PLL_UNLOCK_ERR)) {
		if (en)
			reg |= BIT(28);
		else
			reg &= ~BIT(28);
	}

	DSI_W32(ctrl, 0x10c, reg);
	wmb(); /* ensure error is masked */
}

void dsi_ctrl_hw_cmn_error_intr_ctrl(struct dsi_ctrl_hw *ctrl, bool en)
{
	u32 reg = 0;
	u32 dsi_total_mask = 0x2222AA02;

	reg = DSI_R32(ctrl, 0x110);
	reg &= dsi_total_mask;

	if (en)
		reg |= (BIT(24) | BIT(25));
	else
		reg &= ~BIT(25);

	DSI_W32(ctrl, 0x110, reg);
	wmb(); /* ensure error is masked */
}

u32 dsi_ctrl_hw_cmn_get_error_mask(struct dsi_ctrl_hw *ctrl)
{
	u32 reg = 0;

	reg = DSI_R32(ctrl, 0x10c);

	return reg;
}

u32 dsi_ctrl_hw_cmn_get_hw_version(struct dsi_ctrl_hw *ctrl)
{
	u32 reg = 0;

	reg = DSI_R32(ctrl, 0x0);

	return reg;
}

int dsi_ctrl_hw_cmn_wait_for_cmd_mode_mdp_idle(struct dsi_ctrl_hw *ctrl)
{
	int rc = 0, val = 0;
	u32 cmd_mode_mdp_busy_mask = BIT(2);
	u32 const sleep_us = 2 * 1000;
	u32 const timeout_us = 200 * 1000;

	rc = readl_poll_timeout(ctrl->base + DSI_STATUS, val,
			!(val & cmd_mode_mdp_busy_mask), sleep_us, timeout_us);
	if (rc)
		DSI_CTRL_HW_ERR(ctrl, "timed out waiting for idle\n");

	return rc;
}

void dsi_ctrl_hw_cmn_hs_req_sel(struct dsi_ctrl_hw *ctrl, bool sel_phy)
{
	u32 reg = 0;

	reg = DSI_R32(ctrl, DSI_LANE_CTRL);
	if (sel_phy)
		reg &= ~BIT(24);
	else
		reg |= BIT(24);
	DSI_W32(ctrl, DSI_LANE_CTRL, reg);
	wmb(); /* make sure request is set */
}

void dsi_ctrl_hw_cmn_set_continuous_clk(struct dsi_ctrl_hw *ctrl, bool enable)
{
	u32 reg = 0;

	reg = DSI_R32(ctrl, DSI_LANE_CTRL);
	if (enable)
		reg |= BIT(28);
	else
		reg &= ~BIT(28);
	DSI_W32(ctrl, DSI_LANE_CTRL, reg);
	wmb(); /* make sure request is set */
}

int dsi_ctrl_hw_cmn_wait4dynamic_refresh_done(struct dsi_ctrl_hw *ctrl)
{
	int rc;
	u32 const sleep_us = 1000;
	u32 const timeout_us = 84000; /* approximately 5 vsyncs */
	u32 reg = 0, dyn_refresh_done = BIT(28);

	rc = readl_poll_timeout(ctrl->base + DSI_INT_CTRL, reg,
				(reg & dyn_refresh_done), sleep_us, timeout_us);
	if (rc) {
		DSI_CTRL_HW_ERR(ctrl, "wait4dynamic refresh timedout %d\n", rc);
		return rc;
	}

	/* ack dynamic refresh done status */
	reg = DSI_R32(ctrl, DSI_INT_CTRL);
	reg |= dyn_refresh_done;
	DSI_W32(ctrl, DSI_INT_CTRL, reg);

	return 0;
}


/* A52_PHASE408_DIRECT_BIO_DEBUG_DSI_V1
 * Phase407 filesystem-path transport replaced by direct sda8 BIO.
 * One 4 KiB record, one CRC32C, one commit marker, no duplicate, no R48.
 */
static const char a52_p408_marker[] __used = "A52_PHASE408_DIRECT_BIO_DEBUG_DSI_V1";


/* Phase411 durable Samsung debug2m evidence.
 * stage 11 = exact F0 before dsi_message_setup_tx_mode()
 * stage 12 = exact F0 after mode selection
 * stage 13 = kernel alive 1 second later, immediately before disk flush
 */

/* A52_PHASE413_CONCURRENT_DEBUG2M_MIRROR_V1
 *
 * The 1 MiB RAM recorder remains the hot path. Every committed software event
 * only updates RAM, then nudges an unbound delayed-work mirror. The worker
 * snapshots the current RAM image and writes the 2 MiB CRC32C image to
 * /dev/block/by-name/debug + 0x800000 in parallel with further RAM recording.
 *
 * No submit_bio_wait() is executed from the DSI transaction path.
 */
/* A52_PHASE414_SEQUENTIAL_3M_RECORDER_V1: Phase413 debug2m mirror retired; Phase414 owns the partition. */



/* A52_PHASE411_FIFO_PANIC_SMMU_EXPERIMENT_V1: functional FIFO/panic experiment; observer burst retired. */
static const char a52_p411_hw_marker[] __used = "A52_PHASE411_FIFO_PANIC_SMMU_EXPERIMENT_V1";

/* A52_PHASE413_CONCURRENT_DEBUG2M_MIRROR_V1: background mirror is independent of F0 reachability. */
static const char a52_p413_marker[] __used = "A52_PHASE413_CONCURRENT_DEBUG2M_MIRROR_V1";

/* A52_PHASE414_SEQUENTIAL_3M_RECORDER_V1: Samsung debug2m is now tier 1 of the 3 MiB recorder. */
static const char a52_p414_hwc_marker[] __used = "A52_PHASE414_SEQUENTIAL_3M_RECORDER_V1";

/* A52_PHASE421_FETCH_MEMORY_SURVIVAL_V1: S07/S08 SW_TRIGGER bracket. */

/* A52_PHASE427_CB2_HANDOFF_ROAD_PROBE_V1: S07/pre and post-wait road-to-memory snapshots. */

/* A52_PHASE430_SF_DMA_FORENSICS_V1: corrected TCS + raw DMA timeline. */

/* A52_PHASE434_CLEAN_F0_V1: timer polling retired; active-path F0 state only. */

static const char a52_p439_variant[] __used = "A52_PHASE439_DSI_AUTOPSY_V3:VARIANT_A";

/* A52_PHASE440_EARLY_F0_CLEANUP_FORENSICS_V1: successful early P439 epoch persisted before rearm. */

/* A52_PHASE445_RECOVERY_LADDER_V1: 0..75us matched DSI-local sampler. */
