// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm ADSP/SLPI Peripheral Image Loader for MSM8974 and MSM8996
 *
 * Copyright (C) 2016 Linaro Ltd
 * Copyright (C) 2014 Sony Mobile Communications AB
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/qcom_scm.h>
#include <linux/regulator/consumer.h>
#include <linux/remoteproc.h>
#include <linux/a52_ipa_arm.h>
#include <linux/a52_p470_mss_notif.h>
#include <linux/jiffies.h>
#include "../../a52-compat/include/soc/qcom/subsystem_notif.h"
#include "../../a52-compat/include/soc/qcom/subsystem_restart.h"
#include <linux/a52_mss_ssr_abi.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/smem_state.h>

#include "qcom_common.h"
#include "qcom_pil_info.h"
#include "qcom_q6v5.h"
#include "remoteproc_internal.h"

struct adsp_data {
	int crash_reason_smem;
	const char *firmware_name;
	int pas_id;
	bool has_aggre2_clk;
	bool auto_boot;
	bool a52_level_rails;
	bool a52_mss_rails;

	char **active_pd_names;
	char **proxy_pd_names;

	const char *ssr_name;
	const char *sysmon_name;
	int ssctl_id;
};

struct qcom_adsp {
	struct subsys_desc *a52_modem_ssr_desc;
	struct subsys_device *a52_modem_ssr;
	struct device *dev;
	struct rproc *rproc;

	struct qcom_q6v5 q6v5;

	struct clk *xo;
	struct clk *aggre2_clk;

	struct regulator *cx_supply;
	struct regulator *px_supply;

	struct device *active_pds[1];
	struct device *proxy_pds[3];

	int active_pd_count;
	int proxy_pd_count;

	int pas_id;
	int crash_reason_smem;
	bool has_aggre2_clk;
	bool a52_level_rails;
	bool a52_mss_rails;
	const char *info_name;

	struct completion start_done;
	struct completion stop_done;

	phys_addr_t mem_phys;
	phys_addr_t mem_reloc;
	void *mem_region;
	size_t mem_size;

	struct qcom_rproc_glink glink_subdev;
	struct qcom_rproc_subdev smd_subdev;
	struct qcom_rproc_ssr ssr_subdev;
	struct qcom_sysmon *sysmon;
};

static int adsp_pds_enable(struct qcom_adsp *adsp, struct device **pds,
			   size_t pd_count)
{
	int ret;
	int i;

	for (i = 0; i < pd_count; i++) {
		dev_pm_genpd_set_performance_state(pds[i], INT_MAX);
		ret = pm_runtime_get_sync(pds[i]);
		if (ret < 0) {
			pm_runtime_put_noidle(pds[i]);
			dev_pm_genpd_set_performance_state(pds[i], 0);
			goto unroll_pd_votes;
		}
	}

	return 0;

unroll_pd_votes:
	for (i--; i >= 0; i--) {
		dev_pm_genpd_set_performance_state(pds[i], 0);
		pm_runtime_put(pds[i]);
	}

	return ret;
};

static void adsp_pds_disable(struct qcom_adsp *adsp, struct device **pds,
			     size_t pd_count)
{
	int i;

	for (i = 0; i < pd_count; i++) {
		dev_pm_genpd_set_performance_state(pds[i], 0);
		pm_runtime_put(pds[i]);
	}
}

static int adsp_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;
	int ret;

	ret = qcom_mdt_load(adsp->dev, fw, rproc->firmware, adsp->pas_id,
			    adsp->mem_region, adsp->mem_phys, adsp->mem_size,
			    &adsp->mem_reloc);
	if (ret)
		return ret;

	qcom_pil_info_store(adsp->info_name, adsp->mem_phys, adsp->mem_size);

	return 0;
}

static int adsp_start(struct rproc *rproc)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;
	int ret;

	qcom_q6v5_prepare(&adsp->q6v5);

	ret = adsp_pds_enable(adsp, adsp->active_pds, adsp->active_pd_count);
	if (ret < 0)
		goto disable_irqs;

	ret = adsp_pds_enable(adsp, adsp->proxy_pds, adsp->proxy_pd_count);
	if (ret < 0)
		goto disable_active_pds;

	ret = clk_prepare_enable(adsp->xo);
	if (ret)
		goto disable_proxy_pds;

	ret = clk_prepare_enable(adsp->aggre2_clk);
	if (ret)
		goto disable_xo_clk;

	/* TG lcx.lvl/lmx.lvl level 0x180 (384), same downstream
     * regulator provider as display/GPU. Never add a competing rpmhpd.
     * Provider's maximum encoded performance level is 0xffff.
     */
    if (adsp->a52_level_rails || adsp->a52_mss_rails) {
        ret=regulator_set_voltage(adsp->cx_supply,384,0xffff);
        if (ret) goto disable_aggre2_clk;
        ret=regulator_set_voltage(adsp->px_supply,384,0xffff);
        if (ret) goto disable_aggre2_clk;
        if (adsp->a52_mss_rails)
            dev_info(adsp->dev,"P467 MSS RAILS cx=384 mss=384 rc=0\n");
        else
            dev_info(adsp->dev,"P451 ADSP RAILS lcx=384 lmx=384 rc=0\n");
    }
    ret = regulator_enable(adsp->cx_supply);
	if (ret)
		goto disable_aggre2_clk;

	ret = regulator_enable(adsp->px_supply);
	if (ret)
		goto disable_cx_supply;

	ret = qcom_scm_pas_auth_and_reset(adsp->pas_id);
	if (ret) {
		dev_err(adsp->dev,
			"failed to authenticate image and release reset\n");
		goto disable_px_supply;
	}

	if (adsp->a52_mss_rails)
        dev_info(adsp->dev,"P467 MSS PAS_AUTH rc=0 pas_id=%d fw=%s\n",
                 adsp->pas_id,rproc->firmware);
    else
        dev_info(adsp->dev,"P451 ADSP PAS_AUTH rc=0 pas_id=%d fw=%s\n",
                 adsp->pas_id,rproc->firmware);
    ret = qcom_q6v5_wait_for_start(&adsp->q6v5, msecs_to_jiffies(5000));
	if (ret == -ETIMEDOUT) {
		dev_err(adsp->dev, "start timed out\n");
		qcom_scm_pas_shutdown(adsp->pas_id);
		goto disable_px_supply;
	}

	return 0;

disable_px_supply:
	regulator_disable(adsp->px_supply);
disable_cx_supply:
	regulator_disable(adsp->cx_supply);
disable_aggre2_clk:
    if (adsp->a52_level_rails || adsp->a52_mss_rails) {
        regulator_set_voltage(adsp->px_supply,0,0xffff);
        regulator_set_voltage(adsp->cx_supply,0,0xffff);
    }
	clk_disable_unprepare(adsp->aggre2_clk);
disable_xo_clk:
	clk_disable_unprepare(adsp->xo);
disable_proxy_pds:
	adsp_pds_disable(adsp, adsp->proxy_pds, adsp->proxy_pd_count);
disable_active_pds:
	adsp_pds_disable(adsp, adsp->active_pds, adsp->active_pd_count);
disable_irqs:
	qcom_q6v5_unprepare(&adsp->q6v5);

	return ret;
}

static void qcom_pas_handover(struct qcom_q6v5 *q6v5)
{
	struct qcom_adsp *adsp = container_of(q6v5, struct qcom_adsp, q6v5);

	regulator_disable(adsp->px_supply);
	regulator_disable(adsp->cx_supply);
    if (adsp->a52_level_rails || adsp->a52_mss_rails) {
        regulator_set_voltage(adsp->px_supply,0,0xffff);
        regulator_set_voltage(adsp->cx_supply,0,0xffff);
        if (adsp->a52_mss_rails)
            dev_info(adsp->dev,"P467 MSS RAILS released handover\n");
        else
            dev_info(adsp->dev,"P451 ADSP RAILS released handover\n");
    }
	clk_disable_unprepare(adsp->aggre2_clk);
	clk_disable_unprepare(adsp->xo);
	adsp_pds_disable(adsp, adsp->proxy_pds, adsp->proxy_pd_count);
}

static int adsp_stop(struct rproc *rproc)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;
	int handover;
	int ret;

	ret = qcom_q6v5_request_stop(&adsp->q6v5);
	if (ret == -ETIMEDOUT)
		dev_err(adsp->dev, "timed out on wait\n");

	ret = qcom_scm_pas_shutdown(adsp->pas_id);
	if (ret)
		dev_err(adsp->dev, "failed to shutdown: %d\n", ret);

	adsp_pds_disable(adsp, adsp->active_pds, adsp->active_pd_count);
	handover = qcom_q6v5_unprepare(&adsp->q6v5);
	if (handover)
		qcom_pas_handover(&adsp->q6v5);

	return ret;
}

static void *adsp_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;
	int offset;

	offset = da - adsp->mem_reloc;
	if (offset < 0 || offset + len > adsp->mem_size)
		return NULL;

	return adsp->mem_region + offset;
}

static unsigned long adsp_panic(struct rproc *rproc)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;

	return qcom_q6v5_panic(&adsp->q6v5);
}

static const struct rproc_ops adsp_ops = {
	.start = adsp_start,
	.stop = adsp_stop,
	.da_to_va = adsp_da_to_va,
	.parse_fw = qcom_register_dump_segments,
	.load = adsp_load,
	.panic = adsp_panic,
};

static int adsp_init_clock(struct qcom_adsp *adsp)
{
	int ret;

	adsp->xo = devm_clk_get(adsp->dev, "xo");
	if (IS_ERR(adsp->xo)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get xo clock");
		return ret;
	}

	if (adsp->has_aggre2_clk) {
		adsp->aggre2_clk = devm_clk_get(adsp->dev, "aggre2");
		if (IS_ERR(adsp->aggre2_clk)) {
			ret = PTR_ERR(adsp->aggre2_clk);
			if (ret != -EPROBE_DEFER)
				dev_err(adsp->dev,
					"failed to get aggre2 clock");
			return ret;
		}
	}

	return 0;
}

static int adsp_init_regulator(struct qcom_adsp *adsp)
{
	adsp->cx_supply = devm_regulator_get(adsp->dev, "cx");
	if (IS_ERR(adsp->cx_supply))
		return PTR_ERR(adsp->cx_supply);

	if (!adsp->a52_level_rails)
        regulator_set_load(adsp->cx_supply, 100000);

	adsp->px_supply = devm_regulator_get(adsp->dev, "px");
	return PTR_ERR_OR_ZERO(adsp->px_supply);
}

static int adsp_pds_attach(struct device *dev, struct device **devs,
			   char **pd_names)
{
	size_t num_pds = 0;
	int ret;
	int i;

	if (!pd_names)
		return 0;

	while (pd_names[num_pds])
		num_pds++;

	/* Handle single power domain */
	if (num_pds == 1 && dev->pm_domain) {
		devs[0] = dev;
		pm_runtime_enable(dev);
		return 1;
	}

	for (i = 0; i < num_pds; i++) {
		devs[i] = dev_pm_domain_attach_by_name(dev, pd_names[i]);
		if (IS_ERR_OR_NULL(devs[i])) {
			ret = PTR_ERR(devs[i]) ? : -ENODATA;
			goto unroll_attach;
		}
	}

	return num_pds;

unroll_attach:
	for (i--; i >= 0; i--)
		dev_pm_domain_detach(devs[i], false);

	return ret;
};

static void adsp_pds_detach(struct qcom_adsp *adsp, struct device **pds,
			    size_t pd_count)
{
	struct device *dev = adsp->dev;
	int i;

	/* Handle single power domain */
	if (pd_count == 1 && dev->pm_domain) {
		pm_runtime_disable(dev);
		return;
	}

	for (i = 0; i < pd_count; i++)
		dev_pm_domain_detach(pds[i], false);
}

static int adsp_alloc_memory_region(struct qcom_adsp *adsp)
{
	struct device_node *node;
	struct resource r;
	int ret;

	node = of_parse_phandle(adsp->dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(adsp->dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	of_node_put(node);
	if (ret)
		return ret;

	adsp->mem_phys = adsp->mem_reloc = r.start;
	adsp->mem_size = resource_size(&r);
	adsp->mem_region = devm_ioremap_wc(adsp->dev, adsp->mem_phys, adsp->mem_size);
	if (!adsp->mem_region) {
		dev_err(adsp->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, adsp->mem_size);
		return -EBUSY;
	}

	return 0;
}


/* P468 MSS SUBSYS: TG char ABI. Own ONE rproc power reference for
 * the first legacy subsys_get/open; TG's subsystem_put on the last
 * close balances it. rproc_boot itself detects an already-running rproc
 * and increments its power reference WITHOUT another PAS boot.
 */
static int a52_modem_subsys_powerup(const struct subsys_desc *desc)
{
    struct qcom_adsp *adsp;
    int ret;

    if (!desc || !desc->dev)
        return -ENODEV;
    adsp = dev_get_drvdata(desc->dev);
    if (!adsp || adsp->a52_modem_ssr_desc != desc ||
        adsp->dev != desc->dev || !adsp->rproc)
        return -ENODEV;

    if (READ_ONCE(adsp->rproc->state) == RPROC_CRASHED)
        return -EPIPE;
    /* P469B MSS AUTO_REBOOT_BLOCK: RILD reopening /dev/subsys_modem
     * must not bypass the crashloop guard after the last P468 put.
     * Manual /sys/class/remoteproc/.../state start stays available.
     */
    if (READ_ONCE(adsp->rproc->state) == RPROC_OFFLINE &&
        READ_ONCE(adsp->rproc->recovery_disabled)) {
        dev_warn(adsp->dev,
                 "P469B MSS AUTO_REBOOT_BLOCK offline=1 rc=-EHOSTDOWN\n");
        return -EHOSTDOWN;
    }
    /* P470 MSS IPA_WAIT: only P468's first /dev/subsys_modem open.
     * Do not take a new rproc reference before actual IPA readiness.
     * Timeout leaves P469b manual sysfs start and recovery code unchanged.
     */
    ret = a52_p470_ipa_wait_ready(msecs_to_jiffies(15000));
    if (ret) {
        dev_err(adsp->dev, "P470 MSS WAIT_TIMEOUT modem_not_started rc=%d\n", ret);
        return ret;
    }
    a52_p470_mss_legacy_enter();
    ret = rproc_boot(adsp->rproc);
    a52_p470_mss_legacy_exit();
    if (ret) {
        dev_err(adsp->dev, "P468 MSS SUBSYS OPEN boot_rc=%d\n", ret);
        return ret;
    }
    /* Never report a successful powerup while the PAS is not running. */
    if (READ_ONCE(adsp->rproc->state) != RPROC_RUNNING) {
        dev_err(adsp->dev, "P468 MSS SUBSYS OPEN state_not_running=%d\n",
                READ_ONCE(adsp->rproc->state));
        rproc_shutdown(adsp->rproc); /* undo this open's power ref */
        return -EIO;
    }
    dev_info(adsp->dev, "P468 MSS SUBSYS OPEN boot_rc=0 rproc=RUNNING\n");
    return 0;
}

static int a52_modem_subsys_shutdown(const struct subsys_desc *desc,
                                    bool force_stop)
{
    struct qcom_adsp *adsp;

    if (!desc || !desc->dev)
        return -ENODEV;
    adsp = dev_get_drvdata(desc->dev);
    if (!adsp || adsp->a52_modem_ssr_desc != desc ||
        adsp->dev != desc->dev || !adsp->rproc)
        return -ENODEV;

    /* The SSR core invokes shutdown only on the last subsys put.
     * If manual sysfs still owns a power ref, the MSS remains running.
     */
    a52_p470_mss_legacy_enter();
    rproc_shutdown(adsp->rproc);
    a52_p470_mss_legacy_exit();
    dev_info(adsp->dev, "P468 MSS SUBSYS RELEASE rproc_state=%d force=%d\n",
             READ_ONCE(adsp->rproc->state), force_stop);
    return 0;
}

static int adsp_probe(struct platform_device *pdev)
{
	const struct adsp_data *desc;
	struct qcom_adsp *adsp;
	struct rproc *rproc;
	const char *fw_name;
	int ret;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	fw_name = desc->firmware_name;
	ret = of_property_read_string(pdev->dev.of_node, "firmware-name",
				      &fw_name);
	if (ret < 0 && ret != -EINVAL)
		return ret;

	rproc = rproc_alloc(&pdev->dev, pdev->name, &adsp_ops,
			    fw_name, sizeof(*adsp));
	if (!rproc) {
		dev_err(&pdev->dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	rproc->auto_boot = desc->auto_boot;
    if (desc->a52_mss_rails)
        dev_info(&pdev->dev,
          "P467 MSS PROBE manual_start=1 auto_boot=0 pas=4 smem=421 sysmon=modem ssctl=0x12 shutdown_ack_bit7=UNHANDLED\n");
    if (desc->a52_level_rails)
        dev_info(&pdev->dev,"P451 ADSP PROBE manual_start=1 auto_boot=0 PAS=1\n");
	rproc_coredump_set_elf_info(rproc, ELFCLASS32, EM_NONE);

	adsp = (struct qcom_adsp *)rproc->priv;
	adsp->dev = &pdev->dev;
	adsp->rproc = rproc;
	adsp->pas_id = desc->pas_id;
	adsp->has_aggre2_clk = desc->has_aggre2_clk;
	adsp->a52_level_rails = desc->a52_level_rails;
	adsp->a52_mss_rails = desc->a52_mss_rails;
	adsp->info_name = desc->sysmon_name;
	platform_set_drvdata(pdev, adsp);

	/* P467 MSS + ADSP: the original 5.10 probe called
     * device_wakeup_enable() without device_init_wakeup(). Register the
     * actual rproc parent so rproc_report_crash/pm_stay_awake works.
     */
    ret = device_init_wakeup(adsp->dev, true);
    if (ret) {
        dev_err(adsp->dev,
            "P467 PAS parent wakeup init failed rc=%d\n", ret);
        goto free_rproc;
    }
    dev_info(adsp->dev,
        "P467 PAS PARENT_WAKE enabled=%d pas=%d name=%s\n",
        device_may_wakeup(adsp->dev), adsp->pas_id, dev_name(adsp->dev));

	ret = adsp_alloc_memory_region(adsp);
	if (ret)
		goto free_rproc;

	ret = adsp_init_clock(adsp);
	if (ret)
		goto free_rproc;

	ret = adsp_init_regulator(adsp);
	if (ret)
		goto free_rproc;

	ret = adsp_pds_attach(&pdev->dev, adsp->active_pds,
			      desc->active_pd_names);
	if (ret < 0)
		goto free_rproc;
	adsp->active_pd_count = ret;

	ret = adsp_pds_attach(&pdev->dev, adsp->proxy_pds,
			      desc->proxy_pd_names);
	if (ret < 0)
		goto detach_active_pds;
	adsp->proxy_pd_count = ret;

	ret = qcom_q6v5_init(&adsp->q6v5, pdev, rproc, desc->crash_reason_smem,
			     qcom_pas_handover);
	if (ret)
		goto detach_proxy_pds;

	if (desc->a52_mss_rails)
        dev_info(adsp->dev,
          "P467 MSS GLINK REGISTER edge=glink-edge remote_pid=1 ipcc_client=2 channels=IPCRTR,glink_ssr\n");
    qcom_add_glink_subdev(rproc, &adsp->glink_subdev, desc->ssr_name);
	qcom_add_smd_subdev(rproc, &adsp->smd_subdev);
	qcom_add_ssr_subdev(rproc, &adsp->ssr_subdev, desc->ssr_name);
	adsp->sysmon = qcom_add_sysmon_subdev(rproc,
					      desc->sysmon_name,
					      desc->ssctl_id);
	if (IS_ERR(adsp->sysmon)) {
		ret = PTR_ERR(adsp->sysmon);
		goto detach_proxy_pds;
	}

	ret = rproc_add(rproc);
	if (ret)
		goto detach_proxy_pds;

    if (desc->a52_mss_rails) {
        /* A6b: P468B char shim is optional, PAS rproc stays alive if ABI
         * differs. Allocate from the actual SSR provider translation unit.
         */
        const struct a52_ssr_desc_abi caller_abi = {
        .size = sizeof(struct subsys_desc),
        .name = offsetof(struct subsys_desc, name),
        .dev = offsetof(struct subsys_desc, dev),
        .owner = offsetof(struct subsys_desc, owner),
        .powerup = offsetof(struct subsys_desc, powerup),
        .shutdown = offsetof(struct subsys_desc, shutdown),
        .state = offsetof(struct subsys_desc, state),
        .force_stop_bit = offsetof(struct subsys_desc, force_stop_bit),
        .err_fatal_irq = offsetof(struct subsys_desc, err_fatal_irq),
    };
        struct subsys_desc *ssr;

        dev_info(&pdev->dev,
                 "P470-A6b PAS ABI qcom_adsp_size=%zu dev_offset=%zu ssr_ptr_offset=%zu\n",
                 sizeof(*adsp), offsetof(struct qcom_adsp, dev),
                 offsetof(struct qcom_adsp, a52_modem_ssr_desc));
        ssr = a52_p470_ssr_desc_alloc(&caller_abi);
        if (IS_ERR_OR_NULL(ssr)) {
            int abi_err = IS_ERR(ssr) ? PTR_ERR(ssr) : -ENOMEM;

            dev_err(&pdev->dev,
                    "P470-A6b MSS SSR ABI reject rc=%d P467_PAS_PRESERVED\n",
                    abi_err);
            adsp->a52_modem_ssr_desc = NULL;
            adsp->a52_modem_ssr = NULL;
        } else {
        adsp->a52_modem_ssr_desc = ssr;
        ssr->name = "modem";
        ssr->dev = adsp->dev;
        ssr->owner = THIS_MODULE;
        ssr->powerup = a52_modem_subsys_powerup;
        ssr->shutdown = a52_modem_subsys_shutdown;
        /* Do not set is_not_loadable: real rproc_boot must succeed. */
        dev_info(&pdev->dev,
                 "P470-A6b MSS DEV_CANARY before ok=%d\n",
                 READ_ONCE(adsp->dev) == &pdev->dev);
        if (READ_ONCE(adsp->dev) != &pdev->dev) {
            dev_err(&pdev->dev,
                    "P470-A6b MSS DEV_CANARY CORRUPTED before registration; restoring, shim disabled\n");
            WRITE_ONCE(adsp->dev, &pdev->dev);
            a52_p470_ssr_desc_free(ssr);
            adsp->a52_modem_ssr_desc = NULL;
            adsp->a52_modem_ssr = NULL;
        } else {
        adsp->a52_modem_ssr = subsys_register(ssr);
        dev_info(&pdev->dev,
                 "P470-A6b MSS DEV_CANARY after ok=%d\n",
                 READ_ONCE(adsp->dev) == &pdev->dev);
        if (READ_ONCE(adsp->dev) != &pdev->dev) {
            dev_err(&pdev->dev,
                    "P470-A6b MSS DEV_CANARY CORRUPTED after subsys_register; unregister/free/restore\n");
            WRITE_ONCE(adsp->dev, &pdev->dev);
            if (!IS_ERR_OR_NULL(adsp->a52_modem_ssr))
                subsys_unregister(adsp->a52_modem_ssr);
            adsp->a52_modem_ssr = NULL;
            a52_p470_ssr_desc_free(ssr);
            adsp->a52_modem_ssr_desc = NULL;
        } else {
        if (IS_ERR_OR_NULL(adsp->a52_modem_ssr)) {
            int err = IS_ERR(adsp->a52_modem_ssr) ?
                      PTR_ERR(adsp->a52_modem_ssr) : -ENODEV;
            adsp->a52_modem_ssr = NULL;
            /* No client owns a failed registration. */
            a52_p470_ssr_desc_free(ssr);
            adsp->a52_modem_ssr_desc = NULL;
            dev_err(&pdev->dev,
              "P468 MSS SUBSYS REGISTER failed=%d P467_PAS_PRESERVED\n", err);
        } else {
            dev_info(&pdev->dev,
              "P468 MSS SUBSYS REGISTER name=modem dev=subsys_modem rproc=%s rc=0\n",
              rproc->name);
        }
        } /* stable adsp->dev after SSR registration */
        } /* stable adsp->dev before SSR registration */
        } /* ABI-matched descriptor allocation */
    }
	return 0;

detach_proxy_pds:
	adsp_pds_detach(adsp, adsp->proxy_pds, adsp->proxy_pd_count);
detach_active_pds:
	adsp_pds_detach(adsp, adsp->active_pds, adsp->active_pd_count);
free_rproc:
	device_init_wakeup(adsp->dev, false);
	rproc_free(rproc);

	return ret;
}

static int adsp_remove(struct platform_device *pdev)
{
	struct qcom_adsp *adsp = platform_get_drvdata(pdev);

	if (adsp->a52_modem_ssr) {
		subsys_unregister(adsp->a52_modem_ssr);
		adsp->a52_modem_ssr = NULL;
	}
	a52_p470_ssr_desc_free(adsp->a52_modem_ssr_desc);
	adsp->a52_modem_ssr_desc = NULL;
	rproc_del(adsp->rproc);

	qcom_remove_glink_subdev(adsp->rproc, &adsp->glink_subdev);
	qcom_remove_sysmon_subdev(adsp->sysmon);
	qcom_remove_smd_subdev(adsp->rproc, &adsp->smd_subdev);
	qcom_remove_ssr_subdev(adsp->rproc, &adsp->ssr_subdev);
	adsp_pds_detach(adsp, adsp->proxy_pds, adsp->proxy_pd_count);
	device_init_wakeup(adsp->dev, false);
	rproc_free(adsp->rproc);

	return 0;
}

static const struct adsp_data adsp_resource_init = {
		.crash_reason_smem = 423,
		.firmware_name = "adsp.mdt",
		.pas_id = 1,
		.has_aggre2_clk = false,
		.auto_boot = true,
		.ssr_name = "lpass",
		.sysmon_name = "adsp",
		.ssctl_id = 0x14,
};

static const struct adsp_data sm8150_adsp_resource = {
		.crash_reason_smem = 423,
		.firmware_name = "adsp.mdt",
		.pas_id = 1,
		.has_aggre2_clk = false,
		.auto_boot = true,
		.active_pd_names = (char*[]){
			"load_state",
			NULL
		},
		.proxy_pd_names = (char*[]){
			"cx",
			NULL
		},
		.ssr_name = "lpass",
		.sysmon_name = "adsp",
		.ssctl_id = 0x14,
};

/* A52_PHASE451_SM6350_ADSP_PAS_V1
 * Manual remoteproc bringup only, PAS=1, Samsung/TG A52 LPI regulators.
 * AOSS load_state ID LPASS sends "name: adsp". SSR is "lpass".
 * cx-supply / px-supply in the boot DTB point to TG lcx.lvl/lmx.lvl.
 */
static const struct adsp_data a52_sm6350_adsp_resource = {
    .crash_reason_smem = 423,
    .firmware_name = "adsp.mdt",
    .pas_id = 1,
    .has_aggre2_clk = false,
    .auto_boot = false,
    .a52_level_rails = true,
    .active_pd_names = (char*[]){ "load_state", NULL },
    .ssr_name = "lpass",
    .sysmon_name = "adsp",
    .ssctl_id = 0x14,
};

/* P467 MSS PROBE: manual SM6350 MPSS (TG qcom,mss@4080000).
 * No IPA, modem FastRPC, glinkpkt DATA or ICNSS is enabled.
 * AOSS load_state domain = modem; separate PM6150A S1/S6 level rails.
 */
static const struct adsp_data a52_sm6350_mss_resource = {
    .crash_reason_smem = 421,
    .firmware_name = "modem.mdt",
    .pas_id = 4,
    .has_aggre2_clk = false,
    .auto_boot = false,
    .a52_mss_rails = true,
    .active_pd_names = (char*[]){ "load_state", NULL },
    .ssr_name = "mpss",
    .sysmon_name = "modem",
    .ssctl_id = 0x12,
};

static const struct adsp_data sm8250_adsp_resource = {
	.crash_reason_smem = 423,
	.firmware_name = "adsp.mdt",
	.pas_id = 1,
	.has_aggre2_clk = false,
	.auto_boot = true,
	.active_pd_names = (char*[]){
		"load_state",
		NULL
	},
	.proxy_pd_names = (char*[]){
		"lcx",
		"lmx",
		NULL
	},
	.ssr_name = "lpass",
	.sysmon_name = "adsp",
	.ssctl_id = 0x14,
};

static const struct adsp_data msm8998_adsp_resource = {
		.crash_reason_smem = 423,
		.firmware_name = "adsp.mdt",
		.pas_id = 1,
		.has_aggre2_clk = false,
		.auto_boot = true,
		.proxy_pd_names = (char*[]){
			"cx",
			NULL
		},
		.ssr_name = "lpass",
		.sysmon_name = "adsp",
		.ssctl_id = 0x14,
};

static const struct adsp_data cdsp_resource_init = {
	.crash_reason_smem = 601,
	.firmware_name = "cdsp.mdt",
	.pas_id = 18,
	.has_aggre2_clk = false,
	.auto_boot = true,
	.ssr_name = "cdsp",
	.sysmon_name = "cdsp",
	.ssctl_id = 0x17,
};

static const struct adsp_data sm8150_cdsp_resource = {
	.crash_reason_smem = 601,
	.firmware_name = "cdsp.mdt",
	.pas_id = 18,
	.has_aggre2_clk = false,
	.auto_boot = true,
	.active_pd_names = (char*[]){
		"load_state",
		NULL
	},
	.proxy_pd_names = (char*[]){
		"cx",
		NULL
	},
	.ssr_name = "cdsp",
	.sysmon_name = "cdsp",
	.ssctl_id = 0x17,
};

static const struct adsp_data sm8250_cdsp_resource = {
	.crash_reason_smem = 601,
	.firmware_name = "cdsp.mdt",
	.pas_id = 18,
	.has_aggre2_clk = false,
	.auto_boot = true,
	.active_pd_names = (char*[]){
		"load_state",
		NULL
	},
	.proxy_pd_names = (char*[]){
		"cx",
		NULL
	},
	.ssr_name = "cdsp",
	.sysmon_name = "cdsp",
	.ssctl_id = 0x17,
};

static const struct adsp_data mpss_resource_init = {
	.crash_reason_smem = 421,
	.firmware_name = "modem.mdt",
	.pas_id = 4,
	.has_aggre2_clk = false,
	.auto_boot = false,
	.active_pd_names = (char*[]){
		"load_state",
		NULL
	},
	.proxy_pd_names = (char*[]){
		"cx",
		"mss",
		NULL
	},
	.ssr_name = "mpss",
	.sysmon_name = "modem",
	.ssctl_id = 0x12,
};

static const struct adsp_data slpi_resource_init = {
		.crash_reason_smem = 424,
		.firmware_name = "slpi.mdt",
		.pas_id = 12,
		.has_aggre2_clk = true,
		.auto_boot = true,
		.ssr_name = "dsps",
		.sysmon_name = "slpi",
		.ssctl_id = 0x16,
};

static const struct adsp_data sm8150_slpi_resource = {
		.crash_reason_smem = 424,
		.firmware_name = "slpi.mdt",
		.pas_id = 12,
		.has_aggre2_clk = false,
		.auto_boot = true,
		.active_pd_names = (char*[]){
			"load_state",
			NULL
		},
		.proxy_pd_names = (char*[]){
			"lcx",
			"lmx",
			NULL
		},
		.ssr_name = "dsps",
		.sysmon_name = "slpi",
		.ssctl_id = 0x16,
};

static const struct adsp_data sm8250_slpi_resource = {
	.crash_reason_smem = 424,
	.firmware_name = "slpi.mdt",
	.pas_id = 12,
	.has_aggre2_clk = false,
	.auto_boot = true,
	.active_pd_names = (char*[]){
		"load_state",
		NULL
	},
	.proxy_pd_names = (char*[]){
		"lcx",
		"lmx",
		NULL
	},
	.ssr_name = "dsps",
	.sysmon_name = "slpi",
	.ssctl_id = 0x16,
};

static const struct adsp_data msm8998_slpi_resource = {
		.crash_reason_smem = 424,
		.firmware_name = "slpi.mdt",
		.pas_id = 12,
		.has_aggre2_clk = true,
		.auto_boot = true,
		.proxy_pd_names = (char*[]){
			"ssc_cx",
			NULL
		},
		.ssr_name = "dsps",
		.sysmon_name = "slpi",
		.ssctl_id = 0x16,
};

static const struct adsp_data wcss_resource_init = {
	.crash_reason_smem = 421,
	.firmware_name = "wcnss.mdt",
	.pas_id = 6,
	.auto_boot = true,
	.ssr_name = "mpss",
	.sysmon_name = "wcnss",
	.ssctl_id = 0x12,
};

static const struct of_device_id adsp_of_match[] = {
	{ .compatible = "qcom,msm8974-adsp-pil", .data = &adsp_resource_init},
	{ .compatible = "qcom,msm8996-adsp-pil", .data = &adsp_resource_init},
	{ .compatible = "qcom,msm8996-slpi-pil", .data = &slpi_resource_init},
	{ .compatible = "qcom,msm8998-adsp-pas", .data = &msm8998_adsp_resource},
	{ .compatible = "qcom,msm8998-slpi-pas", .data = &msm8998_slpi_resource},
	{ .compatible = "qcom,qcs404-adsp-pas", .data = &adsp_resource_init },
	{ .compatible = "qcom,qcs404-cdsp-pas", .data = &cdsp_resource_init },
	{ .compatible = "qcom,qcs404-wcss-pas", .data = &wcss_resource_init },
	{ .compatible = "qcom,sc7180-mpss-pas", .data = &mpss_resource_init},
	{ .compatible = "qcom,sdm845-adsp-pas", .data = &adsp_resource_init},
	{ .compatible = "qcom,sdm845-cdsp-pas", .data = &cdsp_resource_init},
	{ .compatible = "qcom,sm8150-adsp-pas", .data = &sm8150_adsp_resource},
	{ .compatible = "qcom,sm8150-cdsp-pas", .data = &sm8150_cdsp_resource},
	{ .compatible = "qcom,sm8150-mpss-pas", .data = &mpss_resource_init},
	{ .compatible = "qcom,sm8150-slpi-pas", .data = &sm8150_slpi_resource},
	{ .compatible = "qcom,sm6350-adsp-pas", .data = &a52_sm6350_adsp_resource},
	{ .compatible = "qcom,sm6350-mss-pas", .data = &a52_sm6350_mss_resource},
	{ .compatible = "qcom,sm8250-adsp-pas", .data = &sm8250_adsp_resource},
	{ .compatible = "qcom,sm8250-cdsp-pas", .data = &sm8250_cdsp_resource},
	{ .compatible = "qcom,sm8250-slpi-pas", .data = &sm8250_slpi_resource},
	{ },
};
MODULE_DEVICE_TABLE(of, adsp_of_match);

static struct platform_driver adsp_driver = {
	.probe = adsp_probe,
	.remove = adsp_remove,
	.driver = {
		.name = "qcom_q6v5_pas",
		.of_match_table = adsp_of_match,
	},
};

module_platform_driver(adsp_driver);
MODULE_DESCRIPTION("Qualcomm Hexagon v5 Peripheral Authentication Service driver");
MODULE_LICENSE("GPL v2");
