#include <linux/mm.h>
#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <linux/a52_phase447.h>
#include <linux/types.h>
#include <linux/dmaengine.h>
#include <linux/msm_gpi.h>
#include <linux/scatterlist.h>
/* A52_PHASE446AA_SE8_DRM_REQUEST_TRACE_V1 */
// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/msm-bus.h>
#include <linux/of_platform.h> /* A52_PHASE446Y_HANDOFF_SE8_V2 */
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/qcom-geni-se.h>
#include <linux/spinlock.h>

#define SE_I2C_TX_TRANS_LEN		0x26c
#define SE_I2C_RX_TRANS_LEN		0x270
#define SE_I2C_SCL_COUNTERS		0x278

#define SE_I2C_ERR  (M_CMD_OVERRUN_EN | M_ILLEGAL_CMD_EN | M_CMD_FAILURE_EN |\
			M_GP_IRQ_1_EN | M_GP_IRQ_3_EN | M_GP_IRQ_4_EN)
#define SE_I2C_ABORT		BIT(1)

/* M_CMD OP codes for I2C */
#define I2C_WRITE		0x1
#define I2C_READ		0x2
#define I2C_WRITE_READ		0x3
#define I2C_ADDR_ONLY		0x4
#define I2C_BUS_CLEAR		0x6
#define I2C_STOP_ON_BUS		0x7
/* M_CMD params for I2C */
#define PRE_CMD_DELAY		BIT(0)
#define TIMESTAMP_BEFORE	BIT(1)
#define STOP_STRETCH		BIT(2)
#define TIMESTAMP_AFTER		BIT(3)
#define POST_COMMAND_DELAY	BIT(4)
#define IGNORE_ADD_NACK		BIT(6)
#define READ_FINISHED_WITH_ACK	BIT(7)
#define BYPASS_ADDR_PHASE	BIT(8)
#define SLV_ADDR_MSK		GENMASK(15, 9)
#define SLV_ADDR_SHFT		9
/* I2C SCL COUNTER fields */
#define HIGH_COUNTER_MSK	GENMASK(29, 20)
#define HIGH_COUNTER_SHFT	20
#define LOW_COUNTER_MSK		GENMASK(19, 10)
#define LOW_COUNTER_SHFT	10
#define CYCLE_COUNTER_MSK	GENMASK(9, 0)

enum geni_i2c_err_code {
	GP_IRQ0,
	NACK,
	GP_IRQ2,
	BUS_PROTO,
	ARB_LOST,
	GP_IRQ5,
	GENI_OVERRUN,
	GENI_ILLEGAL_CMD,
	GENI_ABORT_DONE,
	GENI_TIMEOUT,
};

#define DM_I2C_CB_ERR		((BIT(NACK) | BIT(BUS_PROTO) | BIT(ARB_LOST)) \
									<< 5)

#define I2C_AUTO_SUSPEND_DELAY	250
#define KHZ(freq)		(1000 * freq)
#define PACKING_BYTES_PW	4

#define ABORT_TIMEOUT		HZ
#define XFER_TIMEOUT		HZ
#define RST_TIMEOUT		HZ

struct geni_i2c_dev {
	int a52d_se;
	bool a52ac_gsi;
	struct dma_chan *a52ac_tx, *a52ac_rx;
	bool a52ad_gsi_ready;
	/* A52_PHASE446AF_GPI_CALLBACK_ABI_FIX_V1 */
	struct msm_gpi_dma_async_tx_cb_param a52af_tx_cb, a52af_rx_cb;
	struct device *a52ae_wrapper_dev;
	struct dma_device *a52ae_tx_ops, *a52ae_rx_ops;
	u32 a52ae_gsi_status, a52ae_gsi_code;
	struct msm_gpi_ctrl a52ac_tx_ev, a52ac_rx_ev;

	struct geni_se se;
	u32 tx_wm;
	int irq;
	int err;
	struct i2c_adapter adap;
	struct completion done;
	struct i2c_msg *cur;
	int cur_wr;
	int cur_rd;
	spinlock_t lock;
	u32 clk_freq_out;
	const struct geni_i2c_clk_fld *clk_fld;
	int suspended;
	/* A52_PHASE446Z_QUP_CORE1_TOUCH_FAILFAST_V1 */
	u32 a52z_bus_client;
	bool a52z_bus_active;
	struct msm_bus_scale_pdata a52z_pdata;
	struct msm_bus_paths a52z_paths[2];
	struct msm_bus_vectors a52z_vec[2][2];
	atomic_t a52z_irq_count;
	atomic_t a52y_first_xfer; /* A52_PHASE446Y_HANDOFF_SE8_V2 */
	void *dma_buf;
	size_t xfer_len;
	dma_addr_t dma_addr;
};

struct geni_i2c_err_log {
	int err;
	const char *msg;
};

static const struct geni_i2c_err_log gi2c_log[] = {
	[GP_IRQ0] = {-EIO, "Unknown I2C err GP_IRQ0"},
	[NACK] = {-ENXIO, "NACK: slv unresponsive, check its power/reset-ln"},
	[GP_IRQ2] = {-EIO, "Unknown I2C err GP IRQ2"},
	[BUS_PROTO] = {-EPROTO, "Bus proto err, noisy/unepxected start/stop"},
	[ARB_LOST] = {-EAGAIN, "Bus arbitration lost, clock line undriveable"},
	[GP_IRQ5] = {-EIO, "Unknown I2C err GP IRQ5"},
	[GENI_OVERRUN] = {-EIO, "Cmd overrun, check GENI cmd-state machine"},
	[GENI_ILLEGAL_CMD] = {-EIO, "Illegal cmd, check GENI cmd-state machine"},
	[GENI_ABORT_DONE] = {-ETIMEDOUT, "Abort after timeout successful"},
	[GENI_TIMEOUT] = {-ETIMEDOUT, "I2C TXN timed out"},
};

struct geni_i2c_clk_fld {
	u32	clk_freq_out;
	u8	clk_div;
	u8	t_high_cnt;
	u8	t_low_cnt;
	u8	t_cycle_cnt;
};

/*
 * Hardware uses the underlying formula to calculate time periods of
 * SCL clock cycle. Firmware uses some additional cycles excluded from the
 * below formula and it is confirmed that the time periods are within
 * specification limits.
 *
 * time of high period of SCL: t_high = (t_high_cnt * clk_div) / source_clock
 * time of low period of SCL: t_low = (t_low_cnt * clk_div) / source_clock
 * time of full period of SCL: t_cycle = (t_cycle_cnt * clk_div) / source_clock
 * clk_freq_out = t / t_cycle
 * source_clock = 19.2 MHz
 */
static const struct geni_i2c_clk_fld geni_i2c_clk_map[] = {
	{KHZ(100), 7, 10, 11, 26},
	{KHZ(400), 2,  5, 12, 24},
	{KHZ(1000), 1, 3,  9, 18},
};

static int geni_i2c_clk_map_idx(struct geni_i2c_dev *gi2c)
{
	int i;
	const struct geni_i2c_clk_fld *itr = geni_i2c_clk_map;

	for (i = 0; i < ARRAY_SIZE(geni_i2c_clk_map); i++, itr++) {
		if (itr->clk_freq_out == gi2c->clk_freq_out) {
			gi2c->clk_fld = itr;
			return 0;
		}
	}
	return -EINVAL;
}

static void qcom_geni_i2c_conf(struct geni_i2c_dev *gi2c)
{
	const struct geni_i2c_clk_fld *itr = gi2c->clk_fld;
	u32 val;

	writel_relaxed(0, gi2c->se.base + SE_GENI_CLK_SEL);

	val = (itr->clk_div << CLK_DIV_SHFT) | SER_CLK_EN;
	writel_relaxed(val, gi2c->se.base + GENI_SER_M_CLK_CFG);

	val = itr->t_high_cnt << HIGH_COUNTER_SHFT;
	val |= itr->t_low_cnt << LOW_COUNTER_SHFT;
	val |= itr->t_cycle_cnt;
	writel_relaxed(val, gi2c->se.base + SE_I2C_SCL_COUNTERS);
}

static void geni_i2c_err_misc(struct geni_i2c_dev *gi2c)
{
	u32 m_cmd = readl_relaxed(gi2c->se.base + SE_GENI_M_CMD0);
	u32 m_stat = readl_relaxed(gi2c->se.base + SE_GENI_M_IRQ_STATUS);
	u32 geni_s = readl_relaxed(gi2c->se.base + SE_GENI_STATUS);
	u32 geni_ios = readl_relaxed(gi2c->se.base + SE_GENI_IOS);
	u32 dma = readl_relaxed(gi2c->se.base + SE_GENI_DMA_MODE_EN);
	u32 rx_st, tx_st;

	if (dma) {
		rx_st = readl_relaxed(gi2c->se.base + SE_DMA_RX_IRQ_STAT);
		tx_st = readl_relaxed(gi2c->se.base + SE_DMA_TX_IRQ_STAT);
	} else {
		rx_st = readl_relaxed(gi2c->se.base + SE_GENI_RX_FIFO_STATUS);
		tx_st = readl_relaxed(gi2c->se.base + SE_GENI_TX_FIFO_STATUS);
	}
	dev_dbg(gi2c->se.dev, "DMA:%d tx_stat:0x%x, rx_stat:0x%x, irq-stat:0x%x\n",
		dma, tx_st, rx_st, m_stat);
	dev_dbg(gi2c->se.dev, "m_cmd:0x%x, geni_status:0x%x, geni_ios:0x%x\n",
		m_cmd, geni_s, geni_ios);
}

static void geni_i2c_err(struct geni_i2c_dev *gi2c, int err)
{
	if (!gi2c->err)
		gi2c->err = gi2c_log[err].err;
	if (gi2c->cur)
		dev_dbg(gi2c->se.dev, "len:%d, slv-addr:0x%x, RD/WR:%d\n",
			gi2c->cur->len, gi2c->cur->addr, gi2c->cur->flags);

	if (err != NACK && err != GENI_ABORT_DONE) {
		dev_err(gi2c->se.dev, "%s\n", gi2c_log[err].msg);
		geni_i2c_err_misc(gi2c);
	}
}

static irqreturn_t geni_i2c_irq(int irq, void *dev)
{
	struct geni_i2c_dev *gi2c = dev;
	void __iomem *base = gi2c->se.base;
	int j, p;
	u32 m_stat;
	u32 rx_st;
	u32 dm_tx_st;
	u32 dm_rx_st;
	u32 dma;
	u32 val;
	struct i2c_msg *cur;

	atomic_inc(&gi2c->a52z_irq_count);
	if (gi2c->se.a52_legacy_ahb &&
	    atomic_read(&gi2c->a52z_irq_count) <= 4)
		a52_diag_log_if(false, dev_info, gi2c->se.dev,
			"P446AA SE8 IRQ count=%d raw=%#x enable=%#x status=%#x cmd=%#x\n",
			atomic_read(&gi2c->a52z_irq_count),
			readl_relaxed(base+0x610), readl_relaxed(base+0x614),
			readl_relaxed(base+0x40), readl_relaxed(base+0x600));
	spin_lock(&gi2c->lock);
	m_stat = readl_relaxed(base + SE_GENI_M_IRQ_STATUS);
	rx_st = readl_relaxed(base + SE_GENI_RX_FIFO_STATUS);
	dm_tx_st = readl_relaxed(base + SE_DMA_TX_IRQ_STAT);
	dm_rx_st = readl_relaxed(base + SE_DMA_RX_IRQ_STAT);
	dma = readl_relaxed(base + SE_GENI_DMA_MODE_EN);
	cur = gi2c->cur;

	if (!cur ||
	    m_stat & (M_CMD_FAILURE_EN | M_CMD_ABORT_EN) ||
	    dm_rx_st & (DM_I2C_CB_ERR)) {
		if (m_stat & M_GP_IRQ_1_EN)
			geni_i2c_err(gi2c, NACK);
		if (m_stat & M_GP_IRQ_3_EN)
			geni_i2c_err(gi2c, BUS_PROTO);
		if (m_stat & M_GP_IRQ_4_EN)
			geni_i2c_err(gi2c, ARB_LOST);
		if (m_stat & M_CMD_OVERRUN_EN)
			geni_i2c_err(gi2c, GENI_OVERRUN);
		if (m_stat & M_ILLEGAL_CMD_EN)
			geni_i2c_err(gi2c, GENI_ILLEGAL_CMD);
		if (m_stat & M_CMD_ABORT_EN)
			geni_i2c_err(gi2c, GENI_ABORT_DONE);
		if (m_stat & M_GP_IRQ_0_EN)
			geni_i2c_err(gi2c, GP_IRQ0);

		/* Disable the TX Watermark interrupt to stop TX */
		if (!dma)
			writel_relaxed(0, base + SE_GENI_TX_WATERMARK_REG);
	} else if (dma) {
		dev_dbg(gi2c->se.dev, "i2c dma tx:0x%x, dma rx:0x%x\n",
			dm_tx_st, dm_rx_st);
	} else if (cur->flags & I2C_M_RD &&
		   m_stat & (M_RX_FIFO_WATERMARK_EN | M_RX_FIFO_LAST_EN)) {
		u32 rxcnt = rx_st & RX_FIFO_WC_MSK;

		for (j = 0; j < rxcnt; j++) {
			p = 0;
			val = readl_relaxed(base + SE_GENI_RX_FIFOn);
			while (gi2c->cur_rd < cur->len && p < sizeof(val)) {
				cur->buf[gi2c->cur_rd++] = val & 0xff;
				val >>= 8;
				p++;
			}
			if (gi2c->cur_rd == cur->len)
				break;
		}
	} else if (!(cur->flags & I2C_M_RD) &&
		   m_stat & M_TX_FIFO_WATERMARK_EN) {
		for (j = 0; j < gi2c->tx_wm; j++) {
			u32 temp;

			val = 0;
			p = 0;
			while (gi2c->cur_wr < cur->len && p < sizeof(val)) {
				temp = cur->buf[gi2c->cur_wr++];
				val |= temp << (p * 8);
				p++;
			}
			writel_relaxed(val, base + SE_GENI_TX_FIFOn);
			/* TX Complete, Disable the TX Watermark interrupt */
			if (gi2c->cur_wr == cur->len) {
				writel_relaxed(0, base + SE_GENI_TX_WATERMARK_REG);
				break;
			}
		}
	}

	if (m_stat)
		writel_relaxed(m_stat, base + SE_GENI_M_IRQ_CLEAR);

	if (dma && dm_tx_st)
		writel_relaxed(dm_tx_st, base + SE_DMA_TX_IRQ_CLR);
	if (dma && dm_rx_st)
		writel_relaxed(dm_rx_st, base + SE_DMA_RX_IRQ_CLR);

	/* if this is err with done-bit not set, handle that through timeout. */
	if (m_stat & M_CMD_DONE_EN || m_stat & M_CMD_ABORT_EN ||
	    dm_tx_st & TX_DMA_DONE || dm_tx_st & TX_RESET_DONE ||
	    dm_rx_st & RX_DMA_DONE || dm_rx_st & RX_RESET_DONE)
		complete(&gi2c->done);

	spin_unlock(&gi2c->lock);

	return IRQ_HANDLED;
}


/* A52_PHASE446AB_FIFO_AND_COMPOSER_PROPERTY_EPOCHS_V1
 * SE8-only read-only capability snapshots. Nonzero FIFO_DISABLE_RO
 * indicates no FIFO interface, requiring TG's GPI/GSI path.
 */
static void a52ab_se8_capability(struct geni_i2c_dev *gi2c, const char *where)
{
	static atomic_t first_abort = ATOMIC_INIT(0);
	void __iomem *base=gi2c->se.base;
	u32 param, fifo, dma, depth;
	if (!gi2c->se.a52_legacy_ahb)
		return;
	if (where[0]=='T' && atomic_xchg(&first_abort,1))
		return;
	fifo=readl_relaxed(base+0x64); /* GENI_IF_FIFO_DISABLE_RO */
	dma=readl_relaxed(base+SE_GENI_DMA_MODE_EN); /* 0x258 */
	param=readl_relaxed(base+SE_HW_PARAM_0); /* 0xe24 */
	depth=(param & TX_FIFO_DEPTH_MSK)>>TX_FIFO_DEPTH_SHFT;
	dev_info(gi2c->se.dev,
		"P446AB SE8_CAP stage=%s fifo_disable_ro=%#x dma_mode_en=%#x tx_fifo_depth=%u hw_param0=%#x gsi_only=%d\n",
		where,fifo,dma,depth,param,!!fifo);
}

static void geni_i2c_abort_xfer(struct geni_i2c_dev *gi2c)
{
	u32 val;
	unsigned long time_left = ABORT_TIMEOUT;
	unsigned long flags;

	a52ab_se8_capability(gi2c,"TIMEOUT_BEFORE_ABORT");
	spin_lock_irqsave(&gi2c->lock, flags);
	geni_i2c_err(gi2c, GENI_TIMEOUT);
	gi2c->cur = NULL;
	geni_se_abort_m_cmd(&gi2c->se);
	spin_unlock_irqrestore(&gi2c->lock, flags);
	do {
		time_left = wait_for_completion_timeout(&gi2c->done, time_left);
		val = readl_relaxed(gi2c->se.base + SE_GENI_M_IRQ_STATUS);
	} while (!(val & M_CMD_ABORT_EN) && time_left);

	if (!(val & M_CMD_ABORT_EN))
		dev_err(gi2c->se.dev, "Timeout abort_m_cmd\n");
}

static void geni_i2c_rx_fsm_rst(struct geni_i2c_dev *gi2c)
{
	u32 val;
	unsigned long time_left = RST_TIMEOUT;

	writel_relaxed(1, gi2c->se.base + SE_DMA_RX_FSM_RST);
	do {
		time_left = wait_for_completion_timeout(&gi2c->done, time_left);
		val = readl_relaxed(gi2c->se.base + SE_DMA_RX_IRQ_STAT);
	} while (!(val & RX_RESET_DONE) && time_left);

	if (!(val & RX_RESET_DONE))
		dev_err(gi2c->se.dev, "Timeout resetting RX_FSM\n");
}

static void geni_i2c_tx_fsm_rst(struct geni_i2c_dev *gi2c)
{
	u32 val;
	unsigned long time_left = RST_TIMEOUT;

	writel_relaxed(1, gi2c->se.base + SE_DMA_TX_FSM_RST);
	do {
		time_left = wait_for_completion_timeout(&gi2c->done, time_left);
		val = readl_relaxed(gi2c->se.base + SE_DMA_TX_IRQ_STAT);
	} while (!(val & TX_RESET_DONE) && time_left);

	if (!(val & TX_RESET_DONE))
		dev_err(gi2c->se.dev, "Timeout resetting TX_FSM\n");
}

static void geni_i2c_rx_msg_cleanup(struct geni_i2c_dev *gi2c,
				     struct i2c_msg *cur)
{
	gi2c->cur_rd = 0;
	if (gi2c->dma_buf) {
		if (gi2c->err)
			geni_i2c_rx_fsm_rst(gi2c);
		geni_se_rx_dma_unprep(&gi2c->se, gi2c->dma_addr, gi2c->xfer_len);
		i2c_put_dma_safe_msg_buf(gi2c->dma_buf, cur, !gi2c->err);
	}
}

static void geni_i2c_tx_msg_cleanup(struct geni_i2c_dev *gi2c,
				     struct i2c_msg *cur)
{
	gi2c->cur_wr = 0;
	if (gi2c->dma_buf) {
		if (gi2c->err)
			geni_i2c_tx_fsm_rst(gi2c);
		geni_se_tx_dma_unprep(&gi2c->se, gi2c->dma_addr, gi2c->xfer_len);
		i2c_put_dma_safe_msg_buf(gi2c->dma_buf, cur, !gi2c->err);
	}
}

static int geni_i2c_rx_one_msg(struct geni_i2c_dev *gi2c, struct i2c_msg *msg,
				u32 m_param)
{
	dma_addr_t rx_dma = 0;
	unsigned long time_left;
	void *dma_buf = NULL;
	struct geni_se *se = &gi2c->se;
	size_t len = msg->len;
	struct i2c_msg *cur;

	if (!gi2c->se.a52_legacy_ahb &&
	    !of_machine_is_compatible("lenovo,yoga-c630"))
		dma_buf = i2c_get_dma_safe_msg_buf(msg, 32);

	if (dma_buf)
		geni_se_select_mode(se, GENI_SE_DMA);
	else
		geni_se_select_mode(se, GENI_SE_FIFO);

	writel_relaxed(len, se->base + SE_I2C_RX_TRANS_LEN);
	geni_se_setup_m_cmd(se, I2C_READ, m_param);

	if (dma_buf && geni_se_rx_dma_prep(se, dma_buf, len, &rx_dma)) {
		geni_se_select_mode(se, GENI_SE_FIFO);
		i2c_put_dma_safe_msg_buf(dma_buf, msg, false);
		dma_buf = NULL;
	} else {
		gi2c->xfer_len = len;
		gi2c->dma_addr = rx_dma;
		gi2c->dma_buf = dma_buf;
	}

	cur = gi2c->cur;
	time_left = wait_for_completion_timeout(&gi2c->done, XFER_TIMEOUT);
	if (!time_left)
		geni_i2c_abort_xfer(gi2c);

	geni_i2c_rx_msg_cleanup(gi2c, cur);

	return gi2c->err;
}

static int geni_i2c_tx_one_msg(struct geni_i2c_dev *gi2c, struct i2c_msg *msg,
				u32 m_param)
{
	dma_addr_t tx_dma = 0;
	unsigned long time_left;
	void *dma_buf = NULL;
	struct geni_se *se = &gi2c->se;
	size_t len = msg->len;
	struct i2c_msg *cur;

	if (!gi2c->se.a52_legacy_ahb &&
	    !of_machine_is_compatible("lenovo,yoga-c630"))
		dma_buf = i2c_get_dma_safe_msg_buf(msg, 32);

	if (dma_buf)
		geni_se_select_mode(se, GENI_SE_DMA);
	else
		geni_se_select_mode(se, GENI_SE_FIFO);

	writel_relaxed(len, se->base + SE_I2C_TX_TRANS_LEN);
	geni_se_setup_m_cmd(se, I2C_WRITE, m_param);

	if (dma_buf && geni_se_tx_dma_prep(se, dma_buf, len, &tx_dma)) {
		geni_se_select_mode(se, GENI_SE_FIFO);
		i2c_put_dma_safe_msg_buf(dma_buf, msg, false);
		dma_buf = NULL;
	} else {
		gi2c->xfer_len = len;
		gi2c->dma_addr = tx_dma;
		gi2c->dma_buf = dma_buf;
	}

	if (!dma_buf) /* Get FIFO IRQ */
		writel_relaxed(1, se->base + SE_GENI_TX_WATERMARK_REG);

	cur = gi2c->cur;
	time_left = wait_for_completion_timeout(&gi2c->done, XFER_TIMEOUT);
	if (!time_left)
		geni_i2c_abort_xfer(gi2c);

	geni_i2c_tx_msg_cleanup(gi2c, cur);

	return gi2c->err;
}


/* A52_PHASE446AC_SE8_GSI_I2C_V1
 * For GENI_IF_FIFO_DISABLE_RO=1 the QUP firmware exposes only GSI.
 * Generate TG-compatible CONFIG0/GO/DATA TREs through its GPI DMAengine.
 * Keep the stock ACK FIFO path untouched for all other engines.
 */
/* TG 4.19 GSI mode: ACK 5.10 GENI_SE_DMA is NOT equivalent.
 * Enable external GPI event routing instead of local SE completion IRQs.
 * Ref: TG GSI mode implementation at drivers/platform/msm/qcom-geni-se.c.
 */
static void a52ac_select_gsi_mode(struct geni_i2c_dev *gi2c)
{
	void __iomem *base=gi2c->se.base;
	u32 m_irq=readl_relaxed(base+SE_GENI_M_IRQ_EN);
	u32 s_irq=readl_relaxed(base+SE_GENI_S_IRQ_EN);
	u32 dma=readl_relaxed(base+SE_GENI_DMA_MODE_EN);
	u32 gsi=readl_relaxed(base+0xe18); /* SE_GSI_EVENT_EN */
	/* Local GENI IRQs must not compete with the GPI event ring. */
	writel_relaxed(0,base+0xe1c); /* SE_IRQ_EN */
	m_irq &= ~(M_CMD_DONE_EN|M_TX_FIFO_WATERMARK_EN|
		   M_RX_FIFO_WATERMARK_EN|M_RX_FIFO_LAST_EN);
	s_irq &= ~S_CMD_DONE_EN;
	writel_relaxed(s_irq,base+SE_GENI_S_IRQ_EN);
	writel_relaxed(m_irq,base+SE_GENI_M_IRQ_EN);
	writel_relaxed(0xffffffff,base+SE_GENI_M_IRQ_CLEAR);
	writel_relaxed(0xffffffff,base+SE_GENI_S_IRQ_CLEAR);
	writel_relaxed(0xffffffff,base+SE_DMA_TX_IRQ_CLR);
	writel_relaxed(0xffffffff,base+SE_DMA_RX_IRQ_CLR);
	writel_relaxed(dma|GENI_DMA_MODE_EN,base+SE_GENI_DMA_MODE_EN);
	writel_relaxed(gsi|0xf,base+0xe18); /* RX/TX/M/S GSI events */
	dev_info_once(gi2c->se.dev,
		"P446AC GSI_MODE irq_global=0 events=%#x dma_en=%#x fifo_disable=%#x\n",
		readl_relaxed(base+0xe18),readl_relaxed(base+SE_GENI_DMA_MODE_EN),
		readl_relaxed(base+0x64));
}

static void a52ac_gsi_event(struct dma_chan *ch,
			   const struct msm_gpi_cb *cb, void *arg)
{
	struct geni_i2c_dev *gi2c=arg;
	gi2c->a52ae_gsi_status=cb->status;
	gi2c->a52ae_gsi_code=cb->error_log.error_code;
	if (cb->cb_event==MSM_GPI_QUP_NOTIFY) {
		if (cb->status & M_GP_IRQ_1_EN)
			gi2c->err=-ENXIO;
		else if (cb->status & M_GP_IRQ_3_EN)
			gi2c->err=-EPROTO;
		else if (cb->status & M_GP_IRQ_4_EN)
			gi2c->err=-EAGAIN;
	} else {
		gi2c->err=-EIO;
		dev_err(gi2c->se.dev,
			"P446AC GSI EVENT event=%d status=%#x code=%#x\n",
			cb->cb_event,cb->status,cb->error_log.error_code);
	}
	if (gi2c->err)
		complete(&gi2c->done);
}
static atomic_t a52ag_xfer_count=ATOMIC_INIT(0);
static void a52ac_gsi_tx_cb(void *arg)
{
	struct msm_gpi_dma_async_tx_cb_param *cb=arg;
	struct geni_i2c_dev *gi2c;

	if (unlikely(IS_ERR_OR_NULL(cb) || !virt_addr_valid(cb))) {
		pr_err("P446AF GSI BADPTR field=tx_cb val=%px\n", cb);
		return;
	}
	gi2c=cb->userdata;
	if (unlikely(IS_ERR_OR_NULL(gi2c) || !virt_addr_valid(gi2c))) {
		pr_err("P446AF GSI BADPTR field=tx_cb.userdata val=%px\n", gi2c);
		return;
	}
	if (atomic_read(&a52ag_xfer_count)<=20)
		dev_info_ratelimited(gi2c->se.dev,
		"P446AF GSI TX_CB length=%u code=%u status=%#x\n",
		cb->length,cb->completion_code,cb->status);
	if (cb->completion_code==MSM_GPI_TCE_UNEXP_ERR)
		gi2c->err=-EIO;
	complete(&gi2c->done);
}
static void a52ac_gsi_rx_cb(void *arg)
{
	struct msm_gpi_dma_async_tx_cb_param *cb=arg;
	struct geni_i2c_dev *gi2c;

	if (unlikely(IS_ERR_OR_NULL(cb) || !virt_addr_valid(cb))) {
		pr_err("P446AF GSI BADPTR field=rx_cb val=%px\n", cb);
		return;
	}
	gi2c=cb->userdata;
	if (unlikely(IS_ERR_OR_NULL(gi2c) || !virt_addr_valid(gi2c))) {
		pr_err("P446AF GSI BADPTR field=rx_cb.userdata val=%px\n", gi2c);
		return;
	}
	if (atomic_read(&a52ag_xfer_count)<=20)
		dev_info_ratelimited(gi2c->se.dev,
		"P446AF GSI RX_CB length=%u code=%u status=%#x\n",
		cb->length,cb->completion_code,cb->status);
	if (cb->completion_code==MSM_GPI_TCE_UNEXP_ERR)
		gi2c->err=-EIO;
	complete(&gi2c->done);
}
static void a52ac_gsi_release(void *arg)
{
	struct geni_i2c_dev *gi2c=arg;
	if (!IS_ERR_OR_NULL(gi2c->a52ac_tx)) {
		if (gi2c->a52ad_gsi_ready)
			dmaengine_terminate_all(gi2c->a52ac_tx);
		dma_release_channel(gi2c->a52ac_tx);
		gi2c->a52ac_tx=NULL;
	}
	if (!IS_ERR_OR_NULL(gi2c->a52ac_rx)) {
		if (gi2c->a52ad_gsi_ready)
			dmaengine_terminate_all(gi2c->a52ac_rx);
		dma_release_channel(gi2c->a52ac_rx);
		gi2c->a52ac_rx=NULL;
	}
	gi2c->a52ad_gsi_ready = false;
}
/* A52_PHASE446AD_GSI_FIRST_XFER_GUARD_V1
 * DMA provider readiness is an I2C probe prerequisite, not an FTS xfer task.
 * TG GPI uses five cells: channel, SE ID, protocol, ring size, priority.
 */
static int a52ac_gsi_channels(struct geni_i2c_dev *gi2c)
{
	struct device *dev = gi2c->se.dev;
	struct of_phandle_args txspec, rxspec;
	struct dma_chan *tx, *rx;
	int ret, tx_idx, rx_idx;

	if (!IS_ERR_OR_NULL(gi2c->a52ac_tx) &&
	    !IS_ERR_OR_NULL(gi2c->a52ac_rx))
		return 0;
	if (!dev->of_node ||
	    !of_find_property(dev->of_node, "dmas", NULL) ||
	    !of_find_property(dev->of_node, "dma-names", NULL))
		return dev_err_probe(dev, -EINVAL,
				     "P446AD GSI missing dmas/dma-names\n");
	tx_idx = of_property_match_string(dev->of_node, "dma-names", "tx");
	rx_idx = of_property_match_string(dev->of_node, "dma-names", "rx");
	if (tx_idx < 0 || rx_idx < 0 || tx_idx == rx_idx)
		return dev_err_probe(dev, -EINVAL,
				     "P446AD GSI invalid dma-names tx=%d rx=%d\n",
				     tx_idx, rx_idx);
	ret = of_parse_phandle_with_args(dev->of_node, "dmas", "#dma-cells",
					 tx_idx, &txspec);
	if (ret)
		return dev_err_probe(dev, ret,
				     "P446AD GSI TX spec invalid\n");
	ret = of_parse_phandle_with_args(dev->of_node, "dmas", "#dma-cells",
					 rx_idx, &rxspec);
	if (ret) {
		of_node_put(txspec.np);
		return dev_err_probe(dev, ret,
				     "P446AD GSI RX spec invalid\n");
	}
	if (txspec.np != rxspec.np || txspec.args_count != 5 ||
	    rxspec.args_count != 5 || txspec.args[0] != 0 ||
	    rxspec.args[0] != 1 || !of_device_is_available(txspec.np)) {
		dev_err(dev, "P446AD GSI invalid provider/schema tx=%d rx=%d chid=%u/%u\n",
			txspec.args_count, rxspec.args_count, txspec.args[0],
			rxspec.args[0]);
		of_node_put(txspec.np);
		of_node_put(rxspec.np);
		return -EINVAL;
	}
	dev_info(dev, "P446AD GSI BIND provider=%pOF seid=%u cells=5\n",
		 txspec.np, txspec.args[1]);
	of_node_put(txspec.np);
	of_node_put(rxspec.np);

	tx = dma_request_chan(dev, "tx");
	if (IS_ERR_OR_NULL(tx)) {
		ret = IS_ERR(tx) ? PTR_ERR(tx) : -EPROBE_DEFER;
		dev_warn(dev, "P446AD GSI chan tx=%pe rx=NULL rc=%d stage=request_tx\n",
			 tx, ret);
		return ret;
	}
	gi2c->a52ac_tx = tx;
	gi2c->a52ac_tx_ev.init.callback = a52ac_gsi_event;
	gi2c->a52ac_tx_ev.init.cb_param = gi2c;
	gi2c->a52ac_tx_ev.cmd = MSM_GPI_INIT;
	tx->private = &gi2c->a52ac_tx_ev;

	rx = dma_request_chan(dev, "rx");
	if (IS_ERR_OR_NULL(rx)) {
		ret = IS_ERR(rx) ? PTR_ERR(rx) : -EPROBE_DEFER;
		dev_warn(dev, "P446AD GSI chan tx=%s rx=%pe rc=%d stage=request_rx\n",
			 dma_chan_name(tx), rx, ret);
		goto error;
	}
	gi2c->a52ac_rx = rx;
	gi2c->a52ac_rx_ev.init.callback = a52ac_gsi_event;
	gi2c->a52ac_rx_ev.init.cb_param = gi2c;
	gi2c->a52ac_rx_ev.cmd = MSM_GPI_INIT;
	rx->private = &gi2c->a52ac_rx_ev;

	ret = dmaengine_slave_config(tx, NULL);
	if (ret) {
		dev_err(dev, "P446AD GSI chan tx=%s rx=%s rc=%d stage=config_tx\n",
			dma_chan_name(tx), dma_chan_name(rx), ret);
		goto error;
	}
	ret = dmaengine_slave_config(rx, NULL);
	if (ret) {
		dev_err(dev, "P446AD GSI chan tx=%s rx=%s rc=%d stage=config_rx\n",
			dma_chan_name(tx), dma_chan_name(rx), ret);
		goto error;
	}
	gi2c->a52af_tx_cb.userdata=gi2c;
	gi2c->a52af_rx_cb.userdata=gi2c;
	gi2c->a52ad_gsi_ready = true;
	dev_info(dev, "P446AD GPI READY tx=%s rx=%s ev_ring=initialized rc=0\n",
		 dma_chan_name(tx), dma_chan_name(rx));
	return 0;
error:
	a52ac_gsi_release(gi2c);
	return ret;
}
/* A52_PHASE446AE_GSI_WRAPPER_DMA_V1
 * The SE8 I2C device has no QUP IOMMU SID.  Its wrapper is SID 0x4c3.
 * GPI ring allocations remain under the DMA provider device (SID 0x4d6).
 */
static void a52ae_wrapper_put(void *arg)
{
	put_device((struct device *)arg);
}

static int a52ae_wrapper_bind(struct geni_i2c_dev *gi2c)
{
	struct device *dev = gi2c->se.dev;
	struct device_node *np;
	struct platform_device *wrapper;
	struct iommu_domain *domain;
	struct resource res;
	u32 pool[2];
	int ret;

	if (gi2c->a52ae_wrapper_dev)
		return 0;
	np = of_parse_phandle(dev->of_node, "qcom,wrapper-core", 0);
	if (!np)
		return dev_err_probe(dev, -EINVAL,
				     "P446AE missing SE8 wrapper phandle\n");
	ret = of_address_to_resource(np, 0, &res);
	if (ret || res.start != 0x009c0000) {
		of_node_put(np);
		return dev_err_probe(dev, -EINVAL,
				     "P446AE wrong wrapper address\n");
	}
	ret = of_property_read_u32_array(np, "qcom,iommu-dma-addr-pool",
					 pool, 2);
	if (ret || pool[0] != 0x40000000 || pool[1] != 0xc0000000) {
		dev_err(dev, "P446AE wrapper pool unexpected rc=%d base=%#x size=%#x\n",
			ret, ret ? 0 : pool[0], ret ? 0 : pool[1]);
		of_node_put(np);
		return -EINVAL;
	}
	wrapper = of_find_device_by_node(np);
	of_node_put(np);
	if (!wrapper)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "P446AE QUP wrapper not registered\n");
	domain = iommu_get_domain_for_dev(&wrapper->dev);
	dev_info(dev, "P446AE WRAPPER dev=%s iommu_domain=%p type=%d pool=%#x/%#x\n",
		 dev_name(&wrapper->dev), domain, domain ? domain->type : -1,
		 pool[0], pool[1]);
	if (!domain || domain->type != IOMMU_DOMAIN_DMA) {
		put_device(&wrapper->dev);
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "P446AE QUP wrapper lacks DMA IOMMU domain\n");
	}
	ret = devm_add_action_or_reset(dev, a52ae_wrapper_put, &wrapper->dev);
	if (ret)
		return ret; /* _or_reset releases the platform-device ref */
	gi2c->a52ae_wrapper_dev = &wrapper->dev;
	gi2c->a52ae_tx_ops = gi2c->a52ac_tx->device;
	gi2c->a52ae_rx_ops = gi2c->a52ac_rx->device;
	dev_info(dev, "P446AE DMA_OWNER dev=%s gpi_tx=%s gpi_rx=%s\n",
		 dev_name(gi2c->a52ae_wrapper_dev),
		 dma_chan_name(gi2c->a52ac_tx),
		 dma_chan_name(gi2c->a52ac_rx));
	return 0;
}

static int a52ac_gsi_xfer(struct geni_i2c_dev *gi2c,
			 struct i2c_msg *msgs,int num)
{
	struct device *dev=gi2c->se.dev;
	struct device *mapdev=gi2c->a52ae_wrapper_dev;
	const struct geni_i2c_clk_fld *clk=gi2c->clk_fld;
	int i,ret=0;
	static unsigned long a52ag_last_summary;
	bool a52ag_verbose=(atomic_inc_return(&a52ag_xfer_count)<=20);
	if (!mapdev || !iommu_get_domain_for_dev(mapdev)) {
		dev_err(dev, "P446AE DMA_OWNER missing mapping domain rc=%d\n",-EIO);
		return -EIO;
	}
	if (IS_ERR_OR_NULL(gi2c->a52ac_tx) ||
	    IS_ERR_OR_NULL(gi2c->a52ac_rx)) {
		dev_err(dev, "P446AD I2C GSI first_xfer rc=%d channels absent\n", -EIO);
		return -EIO;
	}
	if (!clk) {
		dev_err(dev, "P446AD I2C GSI first_xfer rc=%d clock timing absent\n", -EIO);
		return -EIO;
	}
	a52ac_select_gsi_mode(gi2c);
	for (i=0;i<num;i++) {
		struct i2c_msg *msg=&msgs[i];
		struct msm_gpi_tre cfg={},go={},data={};
		struct scatterlist txsg[3],rxsg;
		struct dma_async_tx_descriptor *td,*rd;
		dma_addr_t phys;
		u8 *buf;
		u32 a52ac_stretch=(i<num-1) ? 4 : 0;
		u32 a52ac_op=(msg->flags & I2C_M_RD) ? 2 : 1;
		bool rdmsg=!!(msg->flags & I2C_M_RD);
		int n=0;
		unsigned long timeout;
		gi2c->err=0;
		gi2c->a52ae_gsi_status=0;
		gi2c->a52ae_gsi_code=0;
		reinit_completion(&gi2c->done);
		gi2c->cur=msg;
		if (unlikely(IS_ERR_OR_NULL(gi2c->a52ac_tx) ||
			     !virt_addr_valid(gi2c->a52ac_tx) ||
			     IS_ERR_OR_NULL(gi2c->a52ac_rx) ||
			     !virt_addr_valid(gi2c->a52ac_rx))) {
			dev_err(dev, "P446AF GSI BADPTR field=channel tx=%px rx=%px\n",
				gi2c->a52ac_tx, gi2c->a52ac_rx);
			ret=-EIO;
			break;
		}
		if (unlikely(gi2c->a52ac_tx->device != gi2c->a52ae_tx_ops ||
			     gi2c->a52ac_rx->device != gi2c->a52ae_rx_ops)) {
			dev_err(dev, "P446AF GSI BADPTR field=chan.device tx=%px rx=%px\n",
				gi2c->a52ac_tx->device,gi2c->a52ac_rx->device);
			ret=-EIO;
			break;
		}
		buf=i2c_get_dma_safe_msg_buf(msg,1);
		if (!buf) {ret=-ENOMEM;break;}
		phys=dma_map_single(mapdev,buf,msg->len,
				    rdmsg ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
		if (dma_mapping_error(mapdev,phys)) {
			dev_err(dev,"P446AE DMA_MAP dev=%s dir=%s iova=%pad len=%u rc=%d\n",
				dev_name(mapdev),rdmsg ? "rx" : "tx",&phys,msg->len,-EIO);
			dev_err(dev, "P446AD GSI map error rc=%d\n", -EIO);
			i2c_put_dma_safe_msg_buf(buf,msg,false);
			ret=-EIO;
			break;
		}
		/* The old child/parent mapping returned SWIOTLB PA 0xefc00000.
		 * Refuse any such address even if it falls inside the IOVA
		 * numerical pool, which alone does not prove an IOMMU mapping.
		 */
		if (upper_32_bits(phys) || lower_32_bits(phys) < 0x40000000 ||
		    (phys >= 0xefc00000ULL && phys < 0xf0000000ULL) ||
		    msg->len > (0x100000000ULL - phys)) {
			dev_err(dev,"P446AE DMA_MAP dev=%s dir=%s iova=%pad len=%u rc=%d invalid IOVA\n",
				dev_name(mapdev),rdmsg ? "rx" : "tx",&phys,msg->len,-EIO);
			dma_unmap_single(mapdev,phys,msg->len,
				rdmsg ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
			i2c_put_dma_safe_msg_buf(buf,msg,false);
			ret=-EIO;
			break;
		}
		if (a52ag_verbose) a52_diag_log_if(false, dev_info_ratelimited, dev,"P446AE DMA_MAP dev=%s dir=%s iova=%pad len=%u rc=0\n",
			dev_name(mapdev),rdmsg ? "rx" : "tx",&phys,msg->len);
		cfg.dword[0]=MSM_GPI_I2C_CONFIG0_TRE_DWORD0(3,
				clk->t_cycle_cnt,clk->t_high_cnt,clk->t_low_cnt);
		cfg.dword[1]=MSM_GPI_I2C_CONFIG0_TRE_DWORD1(0,0);
		cfg.dword[2]=MSM_GPI_I2C_CONFIG0_TRE_DWORD2(0,clk->clk_div);
		cfg.dword[3]=MSM_GPI_I2C_CONFIG0_TRE_DWORD3(0,0,0,0,1);
		go.dword[0]=MSM_GPI_I2C_GO_TRE_DWORD0(
			    a52ac_stretch,msg->addr,a52ac_op);
		go.dword[1]=MSM_GPI_I2C_GO_TRE_DWORD1;
		go.dword[2]=MSM_GPI_I2C_GO_TRE_DWORD2(rdmsg ? msg->len : 0);
		go.dword[3]=rdmsg ?
			MSM_GPI_I2C_GO_TRE_DWORD3(1,0,0,1,0) :
			MSM_GPI_I2C_GO_TRE_DWORD3(0,0,0,0,1);
		data.dword[0]=MSM_GPI_DMA_W_BUFFER_TRE_DWORD0(phys);
		data.dword[1]=MSM_GPI_DMA_W_BUFFER_TRE_DWORD1(phys);
		data.dword[2]=MSM_GPI_DMA_W_BUFFER_TRE_DWORD2(msg->len);
		data.dword[3]=MSM_GPI_DMA_W_BUFFER_TRE_DWORD3(0,0,1,0,0);
		sg_init_table(txsg,3);
		sg_set_buf(&txsg[n++],&cfg,sizeof(cfg));
		sg_set_buf(&txsg[n++],&go,sizeof(go));
		if (rdmsg) {
			sg_init_table(&rxsg,1);
			sg_set_buf(&rxsg,&data,sizeof(data));
			rd=dmaengine_prep_slave_sg(gi2c->a52ac_rx,&rxsg,1,
					DMA_DEV_TO_MEM,DMA_PREP_INTERRUPT|DMA_CTRL_ACK);
				if (a52ag_verbose) a52_diag_log_if(false, dev_info, dev,"P446AF GSI RX_PREP chan=%px tre=%px sg=%px len=%u\n",
				gi2c->a52ac_rx,&data,&rxsg,msg->len);
			if (IS_ERR_OR_NULL(rd) || !virt_addr_valid(rd)) {
				ret = IS_ERR(rd) ? PTR_ERR(rd) : -EIO;
				dev_err(dev, "P446AD GSI RX descriptor invalid rc=%d\n", ret);
				dev_err(dev, "P446AF GSI BADPTR field=rx_desc val=%px\n",rd);
				goto cleanup;
			}
			rd->callback=a52ac_gsi_rx_cb;
			rd->callback_param=&gi2c->a52af_rx_cb;
			{
				dma_cookie_t cookie=dmaengine_submit(rd);
				ret=dma_submit_error(cookie);
				if (a52ag_verbose) a52_diag_log_if(false, dev_info, dev,
					"P446AF GSI RX_SUBMIT desc=%px cookie=%d rc=%d\n",
					rd,cookie,ret);
				if (ret) goto cleanup;
			}
			dma_async_issue_pending(gi2c->a52ac_rx);
		} else {
			sg_set_buf(&txsg[n++],&data,sizeof(data));
		}
		td=dmaengine_prep_slave_sg(gi2c->a52ac_tx,txsg,n,
				DMA_MEM_TO_DEV,DMA_PREP_INTERRUPT|DMA_CTRL_ACK);
		if (IS_ERR_OR_NULL(td) || !virt_addr_valid(td)) {
			ret = IS_ERR(td) ? PTR_ERR(td) : -EIO;
			dev_err(dev, "P446AD GSI TX descriptor invalid rc=%d\n", ret);
			dev_err(dev, "P446AF GSI BADPTR field=tx_desc val=%px\n",td);
			goto cleanup;
		}
		if (!rdmsg) {
			td->callback=a52ac_gsi_tx_cb;
			td->callback_param=&gi2c->a52af_tx_cb;
		}
		{
			dma_cookie_t cookie=dmaengine_submit(td);
			ret=dma_submit_error(cookie);
			if (a52ag_verbose) a52_diag_log_if(false, dev_info_ratelimited, dev,
				"P446AF GSI TX_SUBMIT desc=%px cookie=%d rc=%d\n",
				td,cookie,ret);
			if (ret)goto cleanup;
		}
		if (a52ag_verbose) a52_diag_log_if(false, dev_info_ratelimited, dev,"P446AC GSI XFER_START msg=%d/%d addr=%#x dir=%c len=%u segs=%d\n",
			i+1,num,msg->addr,rdmsg?'R':'W',msg->len,n);
		dma_async_issue_pending(gi2c->a52ac_tx);
		timeout=wait_for_completion_timeout(&gi2c->done,msecs_to_jiffies(1100));
		if (!timeout) ret=-ETIMEDOUT;
		else if (gi2c->err)ret=gi2c->err;
cleanup:
		if (ret) {
			int stop_rc=-EIO;
			/* The TG GPI I2C terminate op STOP/RESET/STARTs both
			 * channels as one GPII. Never terminate the pair twice.
			 * Do not dereference a possibly damaged dma_device.
			 */
			if (gi2c->a52ad_gsi_ready &&
			    !IS_ERR_OR_NULL(gi2c->a52ac_tx) &&
			    gi2c->a52ac_tx->device == gi2c->a52ae_tx_ops &&
			    !IS_ERR_OR_NULL(gi2c->a52ac_rx) &&
			    gi2c->a52ac_rx->device == gi2c->a52ae_rx_ops)
				stop_rc=dmaengine_terminate_sync(gi2c->a52ac_tx);
			if (stop_rc)
				dev_err(dev,"P446AE GSI ERR status=%#x code=%#x cleanup=terminate_failed rc=%d\n",
					gi2c->a52ae_gsi_status,gi2c->a52ae_gsi_code,stop_rc);
			else
				if (a52ag_verbose) dev_info(dev,"P446AE GSI ERR status=%#x code=%#x cleanup=ok rc=%d\n",
					gi2c->a52ae_gsi_status,gi2c->a52ae_gsi_code,ret);
		}
		dma_unmap_single(mapdev,phys,msg->len,
			rdmsg ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
		i2c_put_dma_safe_msg_buf(buf,msg,!ret);
		if (i == 0)
			if (a52ag_verbose) dev_info(dev, "P446AD I2C GSI first_xfer rc=%d dir=%c len=%u\n",
				 ret, rdmsg ? 'R' : 'W', msg->len);
		if (a52ag_verbose) a52_diag_log_if(ret != 0, dev_info_ratelimited, dev,"P446AC GSI XFER_DONE msg=%d/%d rc=%d\n",i+1,num,ret);
		if (ret)break;
	}
	if (time_after(jiffies,READ_ONCE(a52ag_last_summary)+5*HZ)) {
		WRITE_ONCE(a52ag_last_summary,jiffies);
		dev_info(dev,"P447B GSI SUMMARY xfers=%d rc=%d first20_only=1\n",
			atomic_read(&a52ag_xfer_count),ret);
	}
	return ret;
}

static int geni_i2c_xfer(struct i2c_adapter *adap,
			 struct i2c_msg msgs[],
			 int num)
{
	struct geni_i2c_dev *gi2c = i2c_get_adapdata(adap);
	int i, ret;

	if (gi2c->se.a52_legacy_ahb &&
	    !atomic_read(&gi2c->a52y_first_xfer))
		dev_info(gi2c->se.dev,"P446Z I2C FIRST_BEFORE irq=%d count=%d client=%u vote=%d\n",
			gi2c->irq,atomic_read(&gi2c->a52z_irq_count),
			gi2c->a52z_bus_client,gi2c->a52z_bus_active);
	if (gi2c->se.a52_legacy_ahb && !atomic_read(&gi2c->a52y_first_xfer))
		a52_diag_log_if(false, dev_info, gi2c->se.dev,
			"P446AA SE8 BEFORE se_clk_hz=%lu mclk_cfg=%#x clk_ro=%#x clk_sel=%#x output=%#x irq_en=%#x ios=%#x\n",
			clk_get_rate(gi2c->se.clk),
			readl_relaxed(gi2c->se.base+0x48),
			readl_relaxed(gi2c->se.base+0x60),
			readl_relaxed(gi2c->se.base+0x7c),
			readl_relaxed(gi2c->se.base+0x24),
			readl_relaxed(gi2c->se.base+0x614),
			readl_relaxed(gi2c->se.base+SE_GENI_IOS));
	gi2c->err = 0;
	reinit_completion(&gi2c->done);
	ret = pm_runtime_get_sync(gi2c->se.dev);
	if (ret < 0) {
		dev_err(gi2c->se.dev, "error turning SE resources:%d\n", ret);
		pm_runtime_put_noidle(gi2c->se.dev);
		/* Set device in suspended since resume failed */
		pm_runtime_set_suspended(gi2c->se.dev);
		if (atomic_cmpxchg(&gi2c->a52y_first_xfer,0,1)==0)
			dev_info(gi2c->se.dev,"P446Y I2C FIRST_XFER bus=%d addr=%#x n=%d rc=%d stage=pm_resume\n",
				adap->nr,num?msgs[0].addr:0,num,ret);
		return ret;
	}

	if (gi2c->a52ac_gsi) {
		ret=a52ac_gsi_xfer(gi2c,msgs,num);
		goto a52ac_xfer_done;
	}
	qcom_geni_i2c_conf(gi2c);
	for (i = 0; i < num; i++) {
		u32 m_param = i < (num - 1) ? STOP_STRETCH : 0;

		m_param |= ((msgs[i].addr << SLV_ADDR_SHFT) & SLV_ADDR_MSK);

		gi2c->cur = &msgs[i];
		if (msgs[i].flags & I2C_M_RD)
			ret = geni_i2c_rx_one_msg(gi2c, &msgs[i], m_param);
		else
			ret = geni_i2c_tx_one_msg(gi2c, &msgs[i], m_param);

		if (ret)
			break;
	}
	a52ac_xfer_done:
	if (ret == 0)
		ret = num;

	pm_runtime_mark_last_busy(gi2c->se.dev);
	pm_runtime_put_autosuspend(gi2c->se.dev);
	gi2c->cur = NULL;
	gi2c->err = 0;
	if (atomic_cmpxchg(&gi2c->a52y_first_xfer,0,1)==0) {
		dev_info(gi2c->se.dev,"P446Y I2C FIRST_XFER bus=%d addr=%#x n=%d rc=%d\n",
			adap->nr,num?msgs[0].addr:0,num,ret);
		if (gi2c->se.a52_legacy_ahb)
			dev_info(gi2c->se.dev,"P447D I2C SE%d FIRST_XFER bus=%d addr=%#x mode=%s rc=%d\n",
				gi2c->a52d_se,adap->nr,
				num?msgs[0].addr:0,gi2c->a52ac_gsi?"GSI":"FIFO",ret);
	}
	if (gi2c->se.a52_legacy_ahb &&
	    atomic_read(&gi2c->a52y_first_xfer)==1)
		dev_info_once(gi2c->se.dev,"P446Z I2C FIRST_AFTER irq=%d count=%d irq_status=%#x geni_status=%#x ios=%#x rc=%d\n",
			gi2c->irq,atomic_read(&gi2c->a52z_irq_count),
			readl_relaxed(gi2c->se.base+SE_GENI_M_IRQ_STATUS),
			readl_relaxed(gi2c->se.base+SE_GENI_STATUS),
			readl_relaxed(gi2c->se.base+SE_GENI_IOS),ret);
	if (ret == -ETIMEDOUT)
		dev_err(gi2c->se.dev,"P446Y I2C TIMEOUT bus=%d addr=%#x n=%d icc=absent-legacy-ahb-only\n",
			adap->nr,num?msgs[0].addr:0,num);
	return ret;
}

static u32 geni_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm geni_i2c_algo = {
	.master_xfer	= geni_i2c_xfer,
	.functionality	= geni_i2c_func,
};

#ifdef CONFIG_ACPI
static const struct acpi_device_id geni_i2c_acpi_match[] = {
	{ "QCOM0220"},
	{ },
};
MODULE_DEVICE_TABLE(acpi, geni_i2c_acpi_match);
#endif


/* A52_PHASE446Z_QUP_CORE1_TOUCH_FAILFAST_V1
 * DT wrapper has only two (master,slave) pairs and no usecase table.
 * Synthesize the two-case table, using ONLY the four wrapper DT cells.
 * Values below are B/s for legacy MSM-BUS (ICC kB/s x 1000):
 * CORE_2X_19_2_MHZ=960 kB/s, GENI_DEFAULT_BW=1 kB/s.
 * Never touch the bus controller owned by TZ/ADSP.
 */
#define A52Z_CORE_AB_BPS 960000ULL
#define A52Z_CORE_IB_BPS 960000ULL
#define A52Z_MEM_AB_BPS 1000ULL
#define A52Z_MEM_IB_BPS 1000ULL

static int a52z_bus_vote(struct geni_i2c_dev *gi2c, bool active)
{
	int rc;
	if (!gi2c->a52z_bus_client)
		return -ENODEV;
	if (gi2c->a52z_bus_active == active)
		return 0;
	rc=msm_bus_scale_client_update_request(gi2c->a52z_bus_client,
						active ? 1 : 0);
	a52_diag_log_if(rc != 0, dev_info, gi2c->se.dev,
		"P446Z I2C BUS_VOTE client=%u active=%u case=%u core_ab=%llu core_ib=%llu mem_ab=%llu mem_ib=%llu rc=%d\n",
		gi2c->a52z_bus_client,active,active ? 1 : 0,
		(unsigned long long)(active ? A52Z_CORE_AB_BPS : 0),
		(unsigned long long)(active ? A52Z_CORE_IB_BPS : 0),
		(unsigned long long)(active ? A52Z_MEM_AB_BPS : 0),
		(unsigned long long)(active ? A52Z_MEM_IB_BPS : 0),rc);
	if (!rc)
		gi2c->a52z_bus_active=active;
	return rc;
}

static void a52z_bus_cleanup(void *arg)
{
	struct geni_i2c_dev *gi2c=arg;
	if (!gi2c->a52z_bus_client)
		return;
	if (gi2c->a52z_bus_active)
		a52z_bus_vote(gi2c,false);
	msm_bus_scale_unregister_client(gi2c->a52z_bus_client);
	dev_info(gi2c->se.dev,"P446Z I2C BUS_UNREGISTER client=%u\n",
		gi2c->a52z_bus_client);
	gi2c->a52z_bus_client=0;
	gi2c->a52z_bus_active=false;
}

static int a52z_bus_register(struct geni_i2c_dev *gi2c,
			      struct platform_device *pdev)
{
	struct device *dev=&pdev->dev;
	struct device_node *node;
	struct resource res;
	u32 ids[4],paths;
	int rc,i;
	node=of_parse_phandle(dev->of_node,"qcom,wrapper-core",0);
	if (!node)
		return dev_err_probe(dev,-EINVAL,"P446Z I2C missing wrapper phandle\n");
	rc=of_address_to_resource(node,0,&res);
	if (rc || res.start!=0x009c0000 ||
	    !of_device_is_compatible(node,"qcom,qupv3-geni-se")) {
		dev_err(dev,"P446Z I2C unsupported wrapper rc=%d base=%pa (SE8 only)\n",
			rc,rc ? NULL : &res.start);
		of_node_put(node);
		return -ENODEV;
	}
	rc=of_property_read_u32(node,"qcom,msm-bus,num-paths",&paths);
	if (rc || paths!=2 ||
	    of_property_count_u32_elems(node,"qcom,msm-bus,vectors-bus-ids")!=4) {
		dev_err(dev,"P446Z I2C wrapper bus schema mismatch rc=%d paths=%u\n",
			rc,rc ? 0 : paths);
		of_node_put(node);
		return -EINVAL;
	}
	rc=of_property_read_u32_array(node,"qcom,msm-bus,vectors-bus-ids",ids,4);
	of_node_put(node);
	if (rc || !ids[0] || !ids[1] || !ids[2] || !ids[3] ||
	    (ids[0]==ids[2] && ids[1]==ids[3]))
		return dev_err_probe(dev,-EINVAL,"P446Z I2C wrapper invalid bus IDs rc=%d\n",rc);
	for(i=0;i<2;i++) {
		gi2c->a52z_paths[i].num_paths=2;
		gi2c->a52z_paths[i].vectors=gi2c->a52z_vec[i];
		gi2c->a52z_vec[i][0].src=ids[0];
		gi2c->a52z_vec[i][0].dst=ids[1];
		gi2c->a52z_vec[i][1].src=ids[2];
		gi2c->a52z_vec[i][1].dst=ids[3];
	}
	gi2c->a52z_vec[1][0].ab=A52Z_CORE_AB_BPS;
	gi2c->a52z_vec[1][0].ib=A52Z_CORE_IB_BPS;
	gi2c->a52z_vec[1][1].ab=A52Z_MEM_AB_BPS;
	gi2c->a52z_vec[1][1].ib=A52Z_MEM_IB_BPS;
	gi2c->a52z_pdata.usecase=gi2c->a52z_paths;
	gi2c->a52z_pdata.num_usecases=2;
	gi2c->a52z_pdata.name="P446Z_SE8_QUP_CORE1";
	gi2c->a52z_pdata.active_only=0;
	dev_info(dev,"P446Z I2C BUS_PATHS wrapper=0x9c0000 core=%u->%u mem=%u->%u num_paths=2 cases=2\n",
		ids[0],ids[1],ids[2],ids[3]);
	gi2c->a52z_bus_client=msm_bus_scale_register_client(&gi2c->a52z_pdata);
	if (!gi2c->a52z_bus_client)
		return dev_err_probe(dev,-EPROBE_DEFER,"P446Z I2C msm_bus registration unavailable\n");
	rc=devm_add_action_or_reset(dev,a52z_bus_cleanup,gi2c);
	if (rc)
		return rc;
	rc=a52z_bus_vote(gi2c,true);
	dev_info(dev,"P446Z I2C BUS_REGISTER client=%u rc=%d first_transfer_gated=1\n",
		gi2c->a52z_bus_client,rc);
	return rc;
}

static void a52z_pinctrl_witness(struct geni_i2c_dev *gi2c,int rc)
{
	struct device_node *state,*child;
	const char *function="(not-described)";
	state=of_parse_phandle(gi2c->se.dev->of_node,"pinctrl-0",0);
	if (state) {
		if (of_property_read_string(state,"function",&function)) {
			child=of_get_next_available_child(state,NULL);
			if (child) {
				of_property_read_string(child,"function",&function);
				of_node_put(child);
			}
		}
	}
	dev_info(gi2c->se.dev,
		"P446Z I2C PINCTRL state=default rc=%d node=%pOF function=%s irq=%d irq_count=%d\n",
		rc,state,function,gi2c->irq,
		atomic_read(&gi2c->a52z_irq_count));
	of_node_put(state);
}


/* A52_PHASE447D_SE7_SE8_SE10_I2C_V1
 * Hardware offsets verified against A52 R02:
 * SE7  0x984000 SM5714 MFD 0x49, PCA9468 0x57;
 * SE8  0x988000 STM FTS touchscreen; SE10 0x990000 SM5714 USBPD 0x33.
 * Restrict to the exact legacy QUPv3_1 addresses, never scan.
 * a52.i2c_se=8,7,10 can override; default Kconfig mask 0x580.
 */
static u32 a52d_i2c_se_mask=CONFIG_A52_I2C_SE_ALLOW_DEFAULT;
static int __init a52d_i2c_se_param(char *arg)
{
	char *token;
	u32 next=0, n;
	if (!arg || !*arg)
		return -EINVAL;
	while ((token=strsep(&arg,","))) {
		if (kstrtou32(token,0,&n) || (n!=7 && n!=8 && n!=10))
			return -EINVAL;
		next|=BIT(n);
	}
	/* Never enable any QUP engine outside {7,8,10}. */
	a52d_i2c_se_mask=next;
	return 0;
}
early_param("a52.i2c_se",a52d_i2c_se_param);

static int a52d_i2c_se(phys_addr_t addr)
{
	switch (addr) {
	case 0x00984000: return 7;
	case 0x00988000: return 8;
	case 0x00990000: return 10;
	default: return -1;
	}
}
static bool a52d_i2c_legacy_allowed(phys_addr_t addr)
{
	int n=a52d_i2c_se(addr);
	if (n<0 || !(a52d_i2c_se_mask & BIT(n)))
		return false;
	/* Wave0/1: retain SE8-only allowlist; cmdline may override wave. */
	if (n!=8 && !a52_447_enabled(A52_447_SM5714))
		return false;
	return true;
}


/* A52_PHASE447F_TG_I2C_NR_V1: TG A526B, observed on real device.
 * Only SE7/8/10 are currently allowed by P447D; SE0 never bypasses it.
 * Downstream alias IDs 34/35/36 are absent from the deployed GKI DT;
 * reserve them deterministically instead of dynamic 31/32/33.
 */
static int a52f_tg_i2c_nr(int se)
{
	switch (se) {
	case 0: return 33; /* future W2 NFC only, currently DENIED */
	case 7: return 34; /* SM5714 MFD, FG, PCA9468 */
	case 8: return 35; /* STM touch, KEEP GSI protocol untouched */
	case 10:return 36; /* SM5714 USB-PD */
	default:return -ENODEV;
	}
}

static int geni_i2c_probe(struct platform_device *pdev)
{
	struct geni_i2c_dev *gi2c;
	struct resource *res;
	u32 proto, tx_depth;
	int ret;
	struct device *dev = &pdev->dev;
	struct device_node *wrapper_np;
	struct platform_device *wrapper_pdev;
	bool legacy = of_device_is_compatible(dev->of_node,"qcom,i2c-geni");
	const char *freq_source = "clock-frequency";
	u32 fw_revision;

	/* P447D: isolated legacy SE7/8/10 only, gated to Wave2. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (legacy && (!res || !a52d_i2c_legacy_allowed(res->start))) {
		dev_info(dev,"P447D I2C SE_SKIP addr=%pa allow_mask=%#x wave2=%d rc=%d\n",
			res ? &res->start : NULL,a52d_i2c_se_mask,
			a52_447_enabled(A52_447_SM5714),-ENODEV);
		return -ENODEV;
	}
	gi2c = devm_kzalloc(dev, sizeof(*gi2c), GFP_KERNEL);
	if (!gi2c)
		return -ENOMEM;

    gi2c->se.dev = dev;
    gi2c->a52d_se=legacy ? a52d_i2c_se(res->start) : -1;
    atomic_set(&gi2c->a52y_first_xfer,0);
    atomic_set(&gi2c->a52z_irq_count,0);
    if (legacy) {
        wrapper_np = of_parse_phandle(dev->of_node,"qcom,wrapper-core",0);
        if (!wrapper_np) {
            dev_err(dev,"P446Y I2C missing wrapper-core phandle\n");
            return -EINVAL;
        }
        wrapper_pdev = of_find_device_by_node(wrapper_np);
        of_node_put(wrapper_np);
        if (!wrapper_pdev)
            return -EPROBE_DEFER;
        gi2c->se.wrapper = dev_get_drvdata(&wrapper_pdev->dev);
        put_device(&wrapper_pdev->dev);
        if (!gi2c->se.wrapper)
            return -EPROBE_DEFER;
        gi2c->se.a52_legacy_ahb_clks[0].id = "m-ahb";
        gi2c->se.a52_legacy_ahb_clks[1].id = "s-ahb";
        ret = devm_clk_bulk_get(dev,2,gi2c->se.a52_legacy_ahb_clks);
        if (ret)
            return dev_err_probe(dev,ret,"P446Y I2C SE8 AHB m-ahb/s-ahb unavailable\n");
        gi2c->se.a52_legacy_ahb = true;
        ret=a52z_bus_register(gi2c,pdev);
        if (ret)
            return ret;
    } else {
        gi2c->se.wrapper = dev_get_drvdata(dev->parent);
    }
    if (!gi2c->se.wrapper) {
        return -EPROBE_DEFER;
    }
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	gi2c->se.base = devm_ioremap_resource(dev, res);
	if (IS_ERR(gi2c->se.base))
		return PTR_ERR(gi2c->se.base);

	gi2c->se.clk = devm_clk_get(dev, legacy ? "se-clk" : "se");
	if (IS_ERR(gi2c->se.clk) && !has_acpi_companion(dev))
		return PTR_ERR(gi2c->se.clk);

	ret = device_property_read_u32(dev, "clock-frequency", &gi2c->clk_freq_out);
	if (ret) {
		freq_source = "qcom,clk-freq-out";
		ret = device_property_read_u32(dev, freq_source, &gi2c->clk_freq_out);
	}
	if (ret) {
		dev_info(dev, "Bus frequency not specified, default to 100kHz.\n");
		gi2c->clk_freq_out = KHZ(100);
		freq_source="default-100kHz";
	}

	if (has_acpi_companion(dev))
		ACPI_COMPANION_SET(&gi2c->adap.dev, ACPI_COMPANION(dev));

	dev_info(dev,"P446Y I2C CLOCKS SE8=%d se=%s ahb=%s,%s speed=%u source=%s\n",
		legacy,legacy?"se-clk":"se",
		legacy?"m-ahb(SE)":"wrapper",legacy?"s-ahb(SE)":"wrapper",
		gi2c->clk_freq_out,freq_source);
	gi2c->irq = platform_get_irq(pdev, 0);
	if (gi2c->irq < 0)
		return gi2c->irq;

	ret = geni_i2c_clk_map_idx(gi2c);
	if (ret) {
		dev_err(dev, "Invalid clk frequency %d Hz: %d\n",
			gi2c->clk_freq_out, ret);
		return ret;
	}

	gi2c->adap.algo = &geni_i2c_algo;
	init_completion(&gi2c->done);
	spin_lock_init(&gi2c->lock);
	platform_set_drvdata(pdev, gi2c);
	ret = devm_request_irq(dev, gi2c->irq, geni_i2c_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), gi2c);
	if (ret) {
		dev_err(dev, "Request_irq failed:%d: err:%d\n",
			gi2c->irq, ret);
		return ret;
	}
	i2c_set_adapdata(&gi2c->adap, gi2c);
	gi2c->adap.dev.parent = dev;
	gi2c->adap.dev.of_node = dev->of_node;
	strlcpy(gi2c->adap.name, "Geni-I2C", sizeof(gi2c->adap.name));

	ret = geni_icc_get(&gi2c->se, "qup-memory");
	if (ret)
		return ret;
	/*
	 * Set the bus quota for core and cpu to a reasonable value for
	 * register access.
	 * Set quota for DDR based on bus speed.
	 */
	gi2c->se.icc_paths[GENI_TO_CORE].avg_bw = GENI_DEFAULT_BW;
	gi2c->se.icc_paths[CPU_TO_GENI].avg_bw = GENI_DEFAULT_BW;
	gi2c->se.icc_paths[GENI_TO_DDR].avg_bw = Bps_to_icc(gi2c->clk_freq_out);

	ret = geni_icc_set_bw(&gi2c->se);
	if (ret)
		return ret;

	ret = geni_se_resources_on(&gi2c->se);
	if (ret) {
		dev_err(dev, "Error turning on resources %d\n", ret);
		return ret;
	}
	fw_revision = readl_relaxed(gi2c->se.base + GENI_FW_REVISION_RO);
	proto = geni_se_read_proto(&gi2c->se);
	gi2c->a52ac_gsi=gi2c->se.a52_legacy_ahb &&
		!!readl_relaxed(gi2c->se.base+0x64);
	if (legacy)
		dev_info(dev,"P447D I2C SE%d mode=%s fifo_disable_ro=%#x base=%pa rc=0\n",
			a52d_i2c_se(res->start),gi2c->a52ac_gsi?"GSI":"FIFO",
			readl_relaxed(gi2c->se.base+0x64),&res->start);
	if (gi2c->a52ac_gsi) {
		dev_info(dev, "P446AC GSI_ONLY SE8 fifo_disabled=1 transport=TG_GPI\n");
		ret = devm_add_action_or_reset(dev, a52ac_gsi_release, gi2c);
		if (ret)
			return ret;
		ret = a52ac_gsi_channels(gi2c);
		if (ret)
			return dev_err_probe(dev, ret,
					     "P446AD GSI provider not ready\n");
		ret=a52ae_wrapper_bind(gi2c);
		if (ret)
			return ret;
	}
	a52ab_se8_capability(gi2c,"PROBE_FW_READ");
	dev_info(dev,"P446Y I2C FW_READ fw_revision=0x%08x protocol=%u expected=%u (bootloader-owned, no firmware load)\n",
		fw_revision,proto,GENI_SE_I2C);
	tx_depth = geni_se_get_tx_fifo_depth(&gi2c->se);
	if (proto != GENI_SE_I2C) {
		dev_err(dev, "P446Y I2C WRONG_PROTOCOL fw_revision=0x%08x proto=%u, reject rc=-ENXIO\n",fw_revision,proto);
		geni_se_resources_off(&gi2c->se);
		return -ENXIO;
	}
	if (!gi2c->a52ac_gsi) {
	gi2c->tx_wm = tx_depth - 1;
	geni_se_init(&gi2c->se, gi2c->tx_wm, tx_depth);
	}
	if (!gi2c->a52ac_gsi)
	geni_se_config_packing(&gi2c->se, BITS_PER_BYTE, PACKING_BYTES_PW,
							true, true, true);
	ret = geni_se_resources_off(&gi2c->se);
	if (ret) {
		dev_err(dev, "Error turning off resources %d\n", ret);
		return ret;
	}

	ret = geni_icc_disable(&gi2c->se);
	if (ret)
		return ret;

	dev_dbg(dev, "i2c fifo/se-dma mode. fifo depth:%d\n", tx_depth);

	gi2c->suspended = 1;
	pm_runtime_set_suspended(gi2c->se.dev);
	pm_runtime_set_autosuspend_delay(gi2c->se.dev, I2C_AUTO_SUSPEND_DELAY);
	pm_runtime_use_autosuspend(gi2c->se.dev);
	pm_runtime_enable(gi2c->se.dev);

	/* A52_PHASE447E_DT_ALIAS_AUDIT_V1 -- audit, do not renumber SE8. */
	if (gi2c->se.a52_legacy_ahb)
		dev_info(dev,"P447E I2C SE%d DT_ALIAS=%d rule=upstream_auto_num before_register\n",
			gi2c->a52d_se,of_alias_get_id(dev->of_node,"i2c"));
	/* Explicit TG adapter ID, only after legacy SE whitelist passed.
	 * If busy, fail CLOSED: never silently reassign a different ID and
	 * defeat vendor genfscon. Other platform adapters keep normal aliases.
	 */
	if (IS_ENABLED(CONFIG_A52_447F_TG_I2C_NR) &&
	    gi2c->se.a52_legacy_ahb &&
	    a52f_tg_i2c_nr(gi2c->a52d_se)>0) {
		int nr=a52f_tg_i2c_nr(gi2c->a52d_se);
		gi2c->adap.nr=nr;
		ret=i2c_add_numbered_adapter(&gi2c->adap);
		dev_info(dev,
			 "P447F I2C NR ctrl=%pa se=%d nr=%d alias=%d rc=%d policy=TG_A52\n",
			 &res->start,gi2c->a52d_se,nr,
			 of_alias_get_id(dev->of_node,"i2c"),ret);
	} else {
		ret = i2c_add_adapter(&gi2c->adap);
	}
	if (ret) {
		dev_err(dev, "Error adding i2c adapter %d\n", ret);
		pm_runtime_disable(gi2c->se.dev);
		return ret;
	}

	dev_info(dev,"P446Y I2C PROBE_OK bus=%d base=%pa wrapper=%pOF clk=se-clk,m-ahb,s-ahb speed=%u protocol=%u rc=0\n",
		gi2c->adap.nr,&res->start,dev->of_node,gi2c->clk_freq_out,proto);
	if (legacy)
		dev_info(dev,"P447D I2C SE%d PROBE rc=0 bus=%d mode=%s irq=%d bus_vote=%d dma_owner=%s\n",
			a52d_i2c_se(res->start),gi2c->adap.nr,
			gi2c->a52ac_gsi?"GSI":"FIFO",gi2c->irq,
			gi2c->a52z_bus_active,
			gi2c->a52ac_gsi && gi2c->a52ae_wrapper_dev ?
				dev_name(gi2c->a52ae_wrapper_dev):"N/A");
	dev_dbg(dev, "Geni-I2C adaptor successfully added\n");

	return 0;
}

static int geni_i2c_remove(struct platform_device *pdev)
{
	struct geni_i2c_dev *gi2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&gi2c->adap);
	pm_runtime_disable(gi2c->se.dev);
	return 0;
}

static void geni_i2c_shutdown(struct platform_device *pdev)
{
	struct geni_i2c_dev *gi2c = platform_get_drvdata(pdev);

	/* Make client i2c transfers start failing */
	i2c_mark_adapter_suspended(&gi2c->adap);
}

static int __maybe_unused geni_i2c_runtime_suspend(struct device *dev)
{
	int ret;
	struct geni_i2c_dev *gi2c = dev_get_drvdata(dev);

	if (!gi2c->a52ac_gsi)
		disable_irq(gi2c->irq);
	ret = geni_se_resources_off(&gi2c->se);
	if (ret) {
		enable_irq(gi2c->irq);
		return ret;

	} else {
		gi2c->suspended = 1;
	}

	ret=geni_icc_disable(&gi2c->se);
	if (ret)return ret;
	if (gi2c->se.a52_legacy_ahb)
		return a52z_bus_vote(gi2c,false);
	return 0;
}

static int __maybe_unused geni_i2c_runtime_resume(struct device *dev)
{
	int ret;
	struct geni_i2c_dev *gi2c = dev_get_drvdata(dev);

	if (gi2c->se.a52_legacy_ahb) {
		ret=a52z_bus_vote(gi2c,true);
		if (ret)return ret;
	}
	ret = geni_icc_enable(&gi2c->se);
	if (ret) {
		if (gi2c->se.a52_legacy_ahb)a52z_bus_vote(gi2c,false);
		return ret;
	}

	ret = geni_se_resources_on(&gi2c->se);
	if (gi2c->se.a52_legacy_ahb)a52z_pinctrl_witness(gi2c,ret);
	if (ret) {
		if (gi2c->se.a52_legacy_ahb)a52z_bus_vote(gi2c,false);
		return ret;
	}

	if (!gi2c->a52ac_gsi)
		enable_irq(gi2c->irq);
	gi2c->suspended = 0;
	return 0;
}

static int __maybe_unused geni_i2c_suspend_noirq(struct device *dev)
{
	struct geni_i2c_dev *gi2c = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&gi2c->adap);

	if (!gi2c->suspended) {
		geni_i2c_runtime_suspend(dev);
		pm_runtime_disable(dev);
		pm_runtime_set_suspended(dev);
		pm_runtime_enable(dev);
	}
	return 0;
}

static int __maybe_unused geni_i2c_resume_noirq(struct device *dev)
{
	struct geni_i2c_dev *gi2c = dev_get_drvdata(dev);

	i2c_mark_adapter_resumed(&gi2c->adap);
	return 0;
}

static const struct dev_pm_ops geni_i2c_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(geni_i2c_suspend_noirq, geni_i2c_resume_noirq)
	SET_RUNTIME_PM_OPS(geni_i2c_runtime_suspend, geni_i2c_runtime_resume,
									NULL)
};

static const struct of_device_id geni_i2c_dt_match[] = {
	{ .compatible = "qcom,geni-i2c" },
	{ .compatible = "qcom,i2c-geni" }, /* A52_PHASE446Y_HANDOFF_SE8_V2 */
	{}
};
MODULE_DEVICE_TABLE(of, geni_i2c_dt_match);

static int a52y_i2c_probe(struct platform_device *pdev)
{
    struct resource *res=platform_get_resource(pdev,IORESOURCE_MEM,0);
    struct geni_i2c_dev *gi2c;
    int rc=geni_i2c_probe(pdev);
    gi2c=platform_get_drvdata(pdev);
    dev_info(&pdev->dev,"P446Y I2C PROBE bus=%d base=%pa node=%pOF rc=%d\n",
        rc ? -1 : (gi2c ? gi2c->adap.nr : -1),
        res ? &res->start : NULL,pdev->dev.of_node,rc);
    return rc;
}

static struct platform_driver geni_i2c_driver = {
	.probe  = a52y_i2c_probe,
	.remove = geni_i2c_remove,
	.shutdown = geni_i2c_shutdown,
	.driver = {
		.name = "geni_i2c",
		.pm = &geni_i2c_pm_ops,
		.of_match_table = geni_i2c_dt_match,
		.acpi_match_table = ACPI_PTR(geni_i2c_acpi_match),
	},
};

module_platform_driver(geni_i2c_driver);

MODULE_DESCRIPTION("I2C Controller Driver for GENI based QUP cores");
MODULE_LICENSE("GPL v2");
