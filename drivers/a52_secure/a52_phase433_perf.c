// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE433_CPU_MEM_PERF_V1
 * Passive proof that the OSM CPU domains and Qualcomm memory/L3 devfreq stack
 * are alive. No register writes are performed here.
 */
#include <linux/a52_ack_secure_flight_recorder.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define P433_DOMAINS 2
#define P433_LUT_ROWS 40
#define P433_LUT_OFF 0x110
#define P433_VOLT_OFF 0x114
#define P433_ROW_SIZE 0x20
#define P433_PERF_STATE 0x920
#define P433_MAP_SIZE 0x1000

static const phys_addr_t p433_phys[P433_DOMAINS] = {
	0x18323000ULL, 0x18325800ULL,
};
static const unsigned int p433_targets_s[] = { 20, 60, 120 };
static void __iomem *p433_base[P433_DOMAINS];
static struct delayed_work p433_work;
static unsigned int p433_next;

static u64 p433_ms(void)
{
	return div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC);
}

static void p433_cpu_policy(unsigned int sample, unsigned int cpu)
{
	struct cpufreq_policy *p = cpufreq_cpu_get(cpu);
	const char *gov = "-";
	unsigned int cur = 0;

	if (!p) {
		a52_ackfr_record("P433 CPU s=%u t=%llu cpu=%u no-policy",
				 sample, p433_ms(), cpu);
		return;
	}
	if (p->governor)
		gov = p->governor->name;
	cur = cpufreq_quick_get(cpu);
	a52_ackfr_record("P433 CPU s=%u t=%llu c=%u cur=%u min=%u max=%u g=%.12s",
			 sample, p433_ms(), cpu, cur, p->min, p->max, gov);
	cpufreq_cpu_put(p);
}

static void p433_dump_domain(unsigned int sample, unsigned int d)
{
	unsigned int i;
	u32 en, ps;

	if (!p433_base[d]) {
		a52_ackfr_record("P433 DOM s=%u t=%llu d=%u map=0",
				 sample, p433_ms(), d);
		return;
	}
	en = readl_relaxed(p433_base[d]);
	ps = readl_relaxed(p433_base[d] + P433_PERF_STATE);
	a52_ackfr_record("P433 DOM s=%u t=%llu d=%u en=%08x ps=%08x",
			 sample, p433_ms(), d, en, ps);
	for (i = 0; i < P433_LUT_ROWS; i++) {
		u32 lut = readl_relaxed(p433_base[d] + P433_LUT_OFF +
					i * P433_ROW_SIZE);
		u32 volt = readl_relaxed(p433_base[d] + P433_VOLT_OFF +
					 i * P433_ROW_SIZE);
		a52_ackfr_record("P433 LUT s=%u d=%u i=%u f=%08x v=%08x",
				 sample, d, i, lut, volt);
	}
}

static void p433_bound(unsigned int sample, const char *compat)
{
	struct device_node *np;
	unsigned int total = 0, bound = 0;

	for_each_compatible_node(np, NULL, compat) {
		struct platform_device *pdev;

		total++;
		pdev = of_find_device_by_node(np);
		if (!pdev)
			continue;
		if (pdev->dev.driver)
			bound++;
		put_device(&pdev->dev);
	}
	a52_ackfr_record("P433 B s=%u t=%llu n=%u b=%u c=%.28s",
			 sample, p433_ms(), total, bound, compat);
}

static void p433_sample(unsigned int sample)
{
	static const char * const compat[] = {
		"qcom,cpufreq-hw",
		"qcom,devfreq-fw",
		"qcom,devfreq-fw-voter",
		"qcom,devbw",
		"qcom,bimc-bwmon4",
		"qcom,bimc-bwmon5",
		"qcom,arm-memlat-cpugrp",
		"qcom,arm-memlat-mon",
	};
	unsigned int i;

	p433_cpu_policy(sample, 0);
	p433_cpu_policy(sample, 6);
	for (i = 0; i < P433_DOMAINS; i++)
		p433_dump_domain(sample, i);
	for (i = 0; i < ARRAY_SIZE(compat); i++)
		p433_bound(sample, compat[i]);
}

static void p433_schedule_next(void)
{
	u64 now, target;
	unsigned long delay;

	if (p433_next >= ARRAY_SIZE(p433_targets_s))
		return;
	now = p433_ms();
	target = (u64)p433_targets_s[p433_next] * MSEC_PER_SEC;
	delay = target > now ? msecs_to_jiffies(target - now) : 0;
	schedule_delayed_work(&p433_work, delay);
}

static void p433_workfn(struct work_struct *work)
{
	unsigned int sample = p433_targets_s[p433_next];

	p433_sample(sample);
	p433_next++;
	p433_schedule_next();
}

static int __init a52_p433_perf_init(void)
{
	unsigned int i;

	for (i = 0; i < P433_DOMAINS; i++)
		p433_base[i] = ioremap(p433_phys[i], P433_MAP_SIZE);
	INIT_DELAYED_WORK(&p433_work, p433_workfn);
	a52_ackfr_record("P433 PERF init t=%llu cpu+ddr+l3=1", p433_ms());
	p433_sample(0);
	p433_schedule_next();
	return 0;
}
late_initcall(a52_p433_perf_init);
