// SPDX-License-Identifier: GPL-2.0-only
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

extern void a52_persistent_diag_mark(const char *fmt, ...);

#define A52_MARK3(fmt, ...) do { \
	do { } while (0); \
	do { } while (0); \
	do { } while (0); \
} while (0)

#define A52_VERBOSE(fmt, ...) \
	do { } while (0);

struct a52_scan_ctx {
	unsigned int scan;
	unsigned int count;
};

static unsigned int a52_scan_id;
static unsigned int a52_delayed_round;

static int __maybe_unused a52_prop_len(struct device_node *np, const char *name)
{
	int len = -1;

	if (of_get_property(np, name, &len))
		return len;
	return -1;
}

static int a52_platform_match(struct device *dev)
{
	const char *name = dev_name(dev);
	const char *compat = NULL;

	if (dev->of_node)
		of_property_read_string(dev->of_node, "compatible", &compat);

	return strstr(name, "ufs") || strstr(name, "1d84000") ||
	       strstr(name, "1d87000") || strstr(name, "qmp") ||
	       strstr(name, "rpmh") || strstr(name, "gcc") ||
	       strstr(name, "smmu") || strstr(name, "iommu") ||
	       (compat && (strstr(compat, "ufs") || strstr(compat, "qmp") ||
			   strstr(compat, "rpmh") || strstr(compat, "smmu")));
}

static int a52_platform_cb(struct device *dev, void *data)
{
	struct a52_scan_ctx *ctx = data;
	const char *compat = "<none>";
	const char *status = "<none>";
	const char *driver __maybe_unused = dev->driver ? dev->driver->name : "<unbound>";

	if (!a52_platform_match(dev) || ctx->count >= 48)
		return 0;

	if (dev->of_node) {
		of_property_read_string(dev->of_node, "compatible", &compat);
		of_property_read_string(dev->of_node, "status", &status);
	}

	ctx->count++;
	A52_VERBOSE("PLATFORM scan=%u idx=%u dev=%s driver=%s node=%s status=%s compat=%s parent=%s",
		  ctx->scan, ctx->count, dev_name(dev), driver,
		  dev->of_node ? dev->of_node->full_name : "<none>",
		  status, compat, dev->parent ? dev_name(dev->parent) : "<none>");
	return 0;
}

static int a52_block_cb(struct device *dev, void *data)
{
	struct a52_scan_ctx *ctx = data;
	const char *driver __maybe_unused = dev->driver ? dev->driver->name : "<unbound>";
	const char *type __maybe_unused = dev->type && dev->type->name ? dev->type->name : "<none>";
	const char *name = dev_name(dev);

	if (!(name[0] == 's' && name[1] == 'd') &&
	    strncmp(name, "dm-", 3) && strncmp(name, "mmcblk", 6))
		return 0;
	if (ctx->count >= 48)
		return 1;

	ctx->count++;
	A52_VERBOSE("BLOCK scan=%u idx=%u name=%s devt=%u:%u type=%s driver=%s parent=%s",
		  ctx->scan, ctx->count, dev_name(dev), MAJOR(dev->devt),
		  MINOR(dev->devt), type, driver,
		  dev->parent ? dev_name(dev->parent) : "<none>");
	return 0;
}

static void a52_ufs_trace_node(const char *tag, const char *path,
			       const char *fallback_compatible,
			       unsigned int scan)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct resource *mem;
	const char *status = "<absent>";
	const char *compatible = "<absent>";
	const char *driver = "<unbound>";
	u32 lanes = 0xffffffffU;
	int available;
	int irq = -1;
	int defer_state = 0;
	int clock_names;
	int reset_names;
	int phy_names;

	np = of_find_node_by_path(path);
	if (!np && fallback_compatible)
		np = of_find_compatible_node(NULL, NULL, fallback_compatible);

	if (!np) {
		A52_MARK3("LIVE scan=%u tag=%s node=missing path=%s", scan, tag, path);
		return;
	}

	available = of_device_is_available(np);
	of_property_read_string(np, "status", &status);
	of_property_read_string(np, "compatible", &compatible);
	of_property_read_u32(np, "lanes-per-direction", &lanes);
	clock_names = of_property_count_strings(np, "clock-names");
	reset_names = of_property_count_strings(np, "reset-names");
	phy_names = of_property_count_strings(np, "phy-names");
	pdev = of_find_device_by_node(np);
	if (pdev && pdev->dev.driver)
		driver = pdev->dev.driver->name;
	if (pdev && !pdev->dev.driver)
		defer_state = driver_deferred_probe_check_state(&pdev->dev);
	mem = pdev ? platform_get_resource(pdev, IORESOURCE_MEM, 0) : NULL;
	if (pdev)
		irq = platform_get_irq(pdev, 0);

	A52_MARK3("LIVE scan=%u tag=%s node=%s avail=%d status=%s compat=%s pdev=%s driver=%s defer=%d",
		  scan, tag, np->full_name, available, status, compatible,
		  pdev ? "present" : "none", driver, defer_state);
	A52_MARK3("RESOURCE scan=%u tag=%s mem_start=0x%llx mem_end=0x%llx flags=0x%lx irq=%d lanes=%u",
		  scan, tag,
		  mem ? (unsigned long long)mem->start : 0ULL,
		  mem ? (unsigned long long)mem->end : 0ULL,
		  mem ? mem->flags : 0UL, irq, lanes);
	A52_MARK3("PROPS1 scan=%u tag=%s reg=%d interrupts=%d clocks=%d clock_names=%d resets=%d reset_names=%d",
		  scan, tag, a52_prop_len(np, "reg"),
		  a52_prop_len(np, "interrupts"), a52_prop_len(np, "clocks"),
		  clock_names, a52_prop_len(np, "resets"), reset_names);
	A52_MARK3("PROPS2 scan=%u tag=%s phys=%d phy_names=%d iommus=%d power_domains=%d interconnects=%d freq_table=%d",
		  scan, tag, a52_prop_len(np, "phys"), phy_names,
		  a52_prop_len(np, "iommus"), a52_prop_len(np, "power-domains"),
		  a52_prop_len(np, "interconnects"), a52_prop_len(np, "freq-table-hz"));
	A52_MARK3("SUPPLY scan=%u tag=%s vcc=%d vccq=%d vccq2=%d vdd_hba=%d ref_clk=%d reset_gpio=%d",
		  scan, tag, a52_prop_len(np, "vcc-supply"),
		  a52_prop_len(np, "vccq-supply"), a52_prop_len(np, "vccq2-supply"),
		  a52_prop_len(np, "vdd-hba-supply"),
		  a52_prop_len(np, "ref-clk"), a52_prop_len(np, "reset-gpios"));

	if (pdev)
		put_device(&pdev->dev);
	of_node_put(np);
}

static void a52_ufs_snapshot(const char *reason)
{
	struct a52_scan_ctx platform_ctx = { };
	struct a52_scan_ctx block_ctx = { };
	struct device_node *root;
	const char *model = "<none>";
	const char *compat = "<none>";
	unsigned int scan = ++a52_scan_id;

	platform_ctx.scan = scan;
	block_ctx.scan = scan;
	root = of_find_node_by_path("/");
	if (root) {
		of_property_read_string(root, "model", &model);
		of_property_read_string(root, "compatible", &compat);
	}

	A52_MARK3("SNAPSHOT begin scan=%u reason=%s jiffies=%lu system_state=%d root_model=%s root_compat=%s",
		  scan, reason, jiffies, system_state, model, compat);
	a52_ufs_trace_node("host", "/soc/ufshc@1d84000", "qcom,ufshc", scan);
	a52_ufs_trace_node("phy", "/soc/ufsphy_mem@1d87000", NULL, scan);
	bus_for_each_dev(&platform_bus_type, NULL, &platform_ctx, a52_platform_cb);
	class_for_each_device((struct class *)&block_class, NULL, &block_ctx,
			      a52_block_cb);
	A52_MARK3("SNAPSHOT end scan=%u platform_count=%u block_count=%u",
		  scan, platform_ctx.count, block_ctx.count);

	if (root)
		of_node_put(root);
}

static void a52_ufs_delayed_work(struct work_struct *work)
{
	static const unsigned int delay_ms[] = {
		1500,
		4000,
	};

	a52_delayed_round++;
	if (a52_delayed_round == 1)
		a52_ufs_snapshot("delayed-500ms");
	else if (a52_delayed_round == 2)
		a52_ufs_snapshot("delayed-2s");
	else
		a52_ufs_snapshot("delayed-6s");

	if (a52_delayed_round <= ARRAY_SIZE(delay_ms))
		schedule_delayed_work(to_delayed_work(work),
				      msecs_to_jiffies(
					      delay_ms[a52_delayed_round - 1]));
}

static DECLARE_DELAYED_WORK(a52_ufs_work, a52_ufs_delayed_work);

static int __init a52_ufs_live_trace_init(void)
{
	a52_ufs_snapshot("late-init");
	schedule_delayed_work(&a52_ufs_work, msecs_to_jiffies(500));
	return 0;
}
late_initcall_sync(a52_ufs_live_trace_init);
