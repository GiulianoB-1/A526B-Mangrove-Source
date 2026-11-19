/* A52_PHASE261_QCOM_WDT_OBSERVE_ONLY_V1: no functional watchdog changes */
// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2014, The Linux Foundation. All rights reserved.
 */
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>
#include <linux/of_device.h>
#include <linux/a52_ack_secure_flight_recorder.h>

enum wdt_reg {
	WDT_RST,
	WDT_EN,
	WDT_STS,
	WDT_BARK_TIME,
	WDT_BITE_TIME,
};

#define QCOM_WDT_ENABLE		BIT(0)

static const u32 reg_offset_data_apcs_tmr[] = {
	[WDT_RST] = 0x38,
	[WDT_EN] = 0x40,
	[WDT_STS] = 0x44,
	[WDT_BARK_TIME] = 0x4C,
	[WDT_BITE_TIME] = 0x5C,
};

static const u32 reg_offset_data_kpss[] = {
	[WDT_RST] = 0x4,
	[WDT_EN] = 0x8,
	[WDT_STS] = 0xC,
	[WDT_BARK_TIME] = 0x10,
	[WDT_BITE_TIME] = 0x14,
};

struct qcom_wdt_match_data {
	const u32 *offset;
	bool pretimeout;
	unsigned long fixed_rate;
};

struct qcom_wdt {
	struct watchdog_device	wdd;
	unsigned long		rate;
	void __iomem		*base;
	const u32		*layout;
};

static void __iomem *wdt_addr(struct qcom_wdt *wdt, enum wdt_reg reg)
{
	return wdt->base + wdt->layout[reg];
}

static inline
struct qcom_wdt *to_qcom_wdt(struct watchdog_device *wdd)
{
	return container_of(wdd, struct qcom_wdt, wdd);
}

static irqreturn_t qcom_wdt_isr(int irq, void *arg)
{
	struct watchdog_device *wdd = arg;

	watchdog_notify_pretimeout(wdd);

	return IRQ_HANDLED;
}

static int qcom_wdt_start(struct watchdog_device *wdd)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);
	unsigned int bark = wdd->timeout - wdd->pretimeout;

	writel(0, wdt_addr(wdt, WDT_EN));
	writel(1, wdt_addr(wdt, WDT_RST));
	writel(bark * wdt->rate, wdt_addr(wdt, WDT_BARK_TIME));
	writel(wdd->timeout * wdt->rate, wdt_addr(wdt, WDT_BITE_TIME));
	writel(QCOM_WDT_ENABLE, wdt_addr(wdt, WDT_EN));
	return 0;
}

static int qcom_wdt_stop(struct watchdog_device *wdd)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);

	writel(0, wdt_addr(wdt, WDT_EN));
	return 0;
}

static int qcom_wdt_ping(struct watchdog_device *wdd)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);

	writel(1, wdt_addr(wdt, WDT_RST));
	return 0;
}

static int qcom_wdt_set_timeout(struct watchdog_device *wdd,
				unsigned int timeout)
{
	wdd->timeout = timeout;
	return qcom_wdt_start(wdd);
}

static int qcom_wdt_set_pretimeout(struct watchdog_device *wdd,
				   unsigned int timeout)
{
	wdd->pretimeout = timeout;
	return qcom_wdt_start(wdd);
}

static int qcom_wdt_restart(struct watchdog_device *wdd, unsigned long action,
			    void *data)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);
	u32 timeout;

	/*
	 * Trigger watchdog bite:
	 *    Setup BITE_TIME to be 128ms, and enable WDT.
	 */
	timeout = 128 * wdt->rate / 1000;

	writel(0, wdt_addr(wdt, WDT_EN));
	writel(1, wdt_addr(wdt, WDT_RST));
	writel(timeout, wdt_addr(wdt, WDT_BARK_TIME));
	writel(timeout, wdt_addr(wdt, WDT_BITE_TIME));
	writel(QCOM_WDT_ENABLE, wdt_addr(wdt, WDT_EN));

	/*
	 * Actually make sure the above sequence hits hardware before sleeping.
	 */
	wmb();

	mdelay(150);
	return 0;
}

static const struct watchdog_ops qcom_wdt_ops = {
	.start		= qcom_wdt_start,
	.stop		= qcom_wdt_stop,
	.ping		= qcom_wdt_ping,
	.set_timeout	= qcom_wdt_set_timeout,
	.set_pretimeout	= qcom_wdt_set_pretimeout,
	.restart        = qcom_wdt_restart,
	.owner		= THIS_MODULE,
};

static const struct watchdog_info qcom_wdt_info = {
	.options	= WDIOF_KEEPALIVEPING
			| WDIOF_MAGICCLOSE
			| WDIOF_SETTIMEOUT
			| WDIOF_CARDRESET,
	.identity	= KBUILD_MODNAME,
};

static const struct watchdog_info qcom_wdt_pt_info = {
	.options	= WDIOF_KEEPALIVEPING
			| WDIOF_MAGICCLOSE
			| WDIOF_SETTIMEOUT
			| WDIOF_PRETIMEOUT
			| WDIOF_CARDRESET,
	.identity	= KBUILD_MODNAME,
};

static void qcom_clk_disable_unprepare(void *data)
{
	clk_disable_unprepare(data);
}

static const struct qcom_wdt_match_data match_data_apcs_tmr = {
	.offset = reg_offset_data_apcs_tmr,
	.pretimeout = false,
};

static const struct qcom_wdt_match_data match_data_kpss = {
	.offset = reg_offset_data_kpss,
	.pretimeout = true,
};

static const struct qcom_wdt_match_data match_data_msm = {
	.offset = reg_offset_data_kpss,
	.pretimeout = true,
	.fixed_rate = 32765,
};

static int qcom_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_wdt *wdt;
	struct resource *res;
	struct device_node *np = dev->of_node;
	const struct qcom_wdt_match_data *data;
	u32 percpu_offset;
	int irq, ret;
	struct clk *clk;
	bool running;

	data = of_device_get_match_data(dev);
	if (!data) {
		dev_err(dev, "Unsupported QCOM WDT module\n");
		return -ENODEV;
	}

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOMEM;

	/* We use CPU0's DGT for the watchdog */
	if (of_property_read_u32(np, "cpu-offset", &percpu_offset))
		percpu_offset = 0;

	res->start += percpu_offset;
	res->end += percpu_offset;

	wdt->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	if (data->fixed_rate) {
		/*
		 * The downstream qcom,msm-watchdog binding has no clock
		 * phandle. Qualcomm's vendor driver uses a 32765 Hz counter.
		 */
		wdt->rate = data->fixed_rate;
	} else {
		clk = devm_clk_get(dev, NULL);
		if (IS_ERR(clk)) {
			dev_err(dev, "failed to get input clock\n");
			return PTR_ERR(clk);
		}

		ret = clk_prepare_enable(clk);
		if (ret) {
			dev_err(dev, "failed to setup clock\n");
			return ret;
		}
		ret = devm_add_action_or_reset(dev,
					      qcom_clk_disable_unprepare, clk);
		if (ret)
			return ret;

		wdt->rate = clk_get_rate(clk);
	}

	/*
	 * We use the clock rate to calculate the max timeout, so ensure it's
	 * not zero to avoid a divide-by-zero exception.
	 *
	 * WATCHDOG_CORE assumes units of seconds, if the WDT is clocked such
	 * that it would bite before a second elapses it's usefulness is
	 * limited.  Bail if this is the case.
	 */
	if (wdt->rate == 0 ||
	    wdt->rate > 0x10000000U) {
		dev_err(dev, "invalid clock rate\n");
		return -EINVAL;
	}

	/* check if there is pretimeout support */
	irq = platform_get_irq_optional(pdev, 0);
	if (data->pretimeout && irq > 0) {
		ret = devm_request_irq(dev, irq, qcom_wdt_isr, 0,
				       "wdt_bark", &wdt->wdd);
		if (ret)
			return ret;

		wdt->wdd.info = &qcom_wdt_pt_info;
		wdt->wdd.pretimeout = 1;
	} else {
		if (irq == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		wdt->wdd.info = &qcom_wdt_info;
	}

	wdt->wdd.ops = &qcom_wdt_ops;
	wdt->wdd.min_timeout = 1;
	wdt->wdd.max_timeout = 0x10000000U / wdt->rate;
	wdt->wdd.parent = dev;
	wdt->layout = data->offset;

	running = readl(wdt_addr(wdt, WDT_STS)) & 1;
	if (running)
		wdt->wdd.bootstatus = WDIOF_CARDRESET;

	/*
	 * If 'timeout-sec' unspecified in devicetree, assume a 30 second
	 * default, unless the max timeout is less than 30 seconds, then use
	 * the max instead.
	 */
	wdt->wdd.timeout = min(wdt->wdd.max_timeout, 30U);
	watchdog_init_timeout(&wdt->wdd, 0, dev);

	/*
	 * Firmware may leave the watchdog running. Reprogram it before the
	 * boot timeout expires and tell the watchdog core to keep pinging it
	 * until userspace takes ownership.
	 */
	if (running) {
		qcom_wdt_start(&wdt->wdd);
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
	}

		/* A52_FAILURE_WINDOW_WATCHDOG_DISARM */
		{
			u32 a52_wdt_before;
			u32 a52_wdt_after;

			a52_wdt_before = readl(wdt_addr(wdt, WDT_STS));
			qcom_wdt_stop(&wdt->wdd);
			a52_wdt_after = readl(wdt_addr(wdt, WDT_STS));
			a52_ackfr_record("WDT disarm before=%u after=%u",
					  !!(a52_wdt_before & 1),
					  !!(a52_wdt_after & 1));
			dev_warn(&pdev->dev,
				 "A52 diagnostic: watchdog disabled for manual recovery\n");
			dev_warn(dev, "P462 WDT DIAG_SKIP no_bind_rc=-ENODEV\n");
		return -ENODEV;
		}
	ret = devm_watchdog_register_device(dev, &wdt->wdd);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, wdt);
	return 0;
}

static int __maybe_unused qcom_wdt_suspend(struct device *dev)
{
	struct qcom_wdt *wdt = dev_get_drvdata(dev);

	if (!wdt) {
		dev_warn_once(dev, "P462 WDT QCOM_WDT_SUSPEND unbound drvdata, skip\n");
		return 0;
	}


	if (watchdog_active(&wdt->wdd))
		qcom_wdt_stop(&wdt->wdd);

	return 0;
}

static int __maybe_unused qcom_wdt_resume(struct device *dev)
{
	struct qcom_wdt *wdt = dev_get_drvdata(dev);

	if (!wdt) {
		dev_warn_once(dev, "P462 WDT QCOM_WDT_RESUME unbound drvdata, skip\n");
		return 0;
	}


	if (watchdog_active(&wdt->wdd))
		qcom_wdt_start(&wdt->wdd);

	return 0;
}

static SIMPLE_DEV_PM_OPS(qcom_wdt_pm_ops, qcom_wdt_suspend, qcom_wdt_resume);

static const struct of_device_id qcom_wdt_of_table[] = {
	{ .compatible = "qcom,msm-watchdog", .data = &match_data_msm },
	{ .compatible = "qcom,kpss-timer", .data = &match_data_apcs_tmr },
	{ .compatible = "qcom,scss-timer", .data = &match_data_apcs_tmr },
	{ .compatible = "qcom,kpss-wdt", .data = &match_data_kpss },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_wdt_of_table);

static struct platform_driver qcom_watchdog_driver = {
	.probe	= qcom_wdt_probe,
	.driver	= {
		.name		= KBUILD_MODNAME,
		.of_match_table	= qcom_wdt_of_table,
		.pm		= &qcom_wdt_pm_ops,
	},
};
module_platform_driver(qcom_watchdog_driver);

MODULE_DESCRIPTION("QCOM KPSS Watchdog Driver");
MODULE_LICENSE("GPL v2");
