/*
 * Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/component.h>
#include <linux/a52_ack_secure_flight_recorder.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/dma-mapping.h>

#include <asm/dma-iommu.h>
#include <soc/qcom/secure_buffer.h>

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_mmu.h"
#include "sde_dbg.h"

#if defined(CONFIG_DISPLAY_SAMSUNG)
#include "ss_dsi_panel_common.h"
#endif

struct msm_smmu_client {
	struct device *dev;
	struct iommu_domain *domain;
	const struct dma_map_ops *dma_ops;
	bool domain_attached;
	bool secure;
};

struct msm_smmu {
	struct msm_mmu base;
	struct device *client_dev;
	struct msm_smmu_client *client;
	bool client_dev_owned;
};

struct msm_smmu_domain {
	const char *label;
	bool secure;
};

#define to_msm_smmu(x) container_of(x, struct msm_smmu, base)
#define msm_smmu_to_client(smmu) (smmu->client)

extern void a52_p446_mark(u32 event,u32 aux0,u32 aux1);
/* A52_PHASE446B_H03_H04_ATTACH */
static int msm_smmu_attach(struct msm_mmu *mmu, const char * const *names,
		int cnt)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);
	int rc = 0;

	if (!client) {
		pr_err("undefined smmu client\n");
		return -EINVAL;
	}

	/* domain attach only once */
	if (client->domain_attached)
		return 0;

	if (client->dma_ops) {
		set_dma_ops(client->dev, client->dma_ops);
		client->dma_ops = NULL;
		dev_dbg(client->dev, "iommu domain ops restored\n");
	}

	a52_p446_mark(0x123U, client->secure ? 1U : 0U, 0U);
	rc = iommu_attach_device(client->domain, client->dev);
	a52_p446_mark(0x124U, client->secure ? 1U : 0U, (u32)rc);
	if (rc) {
		dev_err(client->dev, "iommu attach dev failed (%d)\n", rc);
		return rc;
	}

	client->domain_attached = true;

	dev_dbg(client->dev, "iommu domain attached\n");

	return 0;
}

static void msm_smmu_detach(struct msm_mmu *mmu, const char * const *names,
		int cnt)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);

	if (!client) {
		pr_err("undefined smmu client\n");
		return;
	}

	if (!client->domain_attached)
		return;

	pm_runtime_get_sync(mmu->dev);
	msm_dma_unmap_all_for_dev(client->dev);
	iommu_detach_device(client->domain, client->dev);

	client->dma_ops = get_dma_ops(client->dev);
	if (client->dma_ops) {
		set_dma_ops(client->dev, NULL);
		dev_dbg(client->dev, "iommu domain ops removed\n");
	}

	pm_runtime_put_sync(mmu->dev);

	client->domain_attached = false;
	dev_dbg(client->dev, "iommu domain detached\n");
}

static int msm_smmu_set_attribute(struct msm_mmu *mmu,
		enum iommu_attr attr, void *data)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);
	int ret = 0;

	if (!client || !client->domain)
		return -ENODEV;

	ret = iommu_domain_set_attr(client->domain, attr, data);
	if (ret)
		DRM_ERROR("set domain attribute failed:%d\n", ret);

	return ret;
}

extern void a52_p446_note_iova(u32 event,u64 iova,u64 phys,u32 aux);
/* A52_PHASE446_IOVA_CHECK */
static int msm_smmu_one_to_one_unmap(struct msm_mmu *mmu,
				uint32_t dest_address, uint32_t size)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);
	int ret = 0;

	if (!client || !client->domain)
		return -ENODEV;

	ret = iommu_unmap(client->domain, dest_address, size);
	if (ret != size)
		pr_err("smmu unmap failed\n");

	return 0;
}

static int msm_smmu_one_to_one_map(struct msm_mmu *mmu, uint32_t iova,
		uint32_t dest_address, uint32_t size, int prot)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);
	int ret = 0;

	a52_ackfr_record("SMMU map client=%d domain=%d base=0x%x size=0x%x",
		!!client, client && client->domain, dest_address, size);
	if (!client || !client->domain) {
		a52_ackfr_record("SMMU map fail rc=%d client=%d domain=%d",
			-ENODEV, !!client, client && client->domain);
		return -ENODEV;
	}

	a52_p446_note_iova(10U,dest_address,iommu_iova_to_phys(client->domain,dest_address),size);
	ret = iommu_map(client->domain, dest_address, dest_address,
			size, prot);
	a52_p446_note_iova(11U,dest_address,iommu_iova_to_phys(client->domain,dest_address),size);
	a52_ackfr_record("SMMU map exit rc=%d", ret);
	if (ret)
		pr_err("smmu map failed\n");

	return ret;
}

static int msm_smmu_map(struct msm_mmu *mmu, uint64_t iova,
		struct sg_table *sgt, unsigned int len, int prot)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);
	size_t ret = 0;

	if (sgt && sgt->sgl) {
		ret = iommu_map_sg(client->domain, iova, sgt->sgl,
				sgt->nents, prot);
		WARN_ON((int)ret < 0);
		DRM_DEBUG("%pad/0x%x/0x%x/\n", &sgt->sgl->dma_address,
				sgt->sgl->dma_length, prot);
		SDE_EVT32(sgt->sgl->dma_address, sgt->sgl->dma_length, prot);
	}
    if (ret != len) {
        static atomic_t errors = ATOMIC_INIT(0);
        int n=atomic_inc_return(&errors);
        if (n<=12)
            pr_warn("P446Q SMMU_MAP_FAIL n=%d iova=%llx mapped=%zu expected=%u sgt_nents=%u prot=%x\n",
                n,(unsigned long long)iova,ret,len,
                sgt ? sgt->nents : 0,prot);
    }
    return (ret == len) ? 0 : -EINVAL;
}

static int msm_smmu_unmap(struct msm_mmu *mmu, uint64_t iova,
		struct sg_table *sgt, unsigned int len)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);

	pm_runtime_get_sync(mmu->dev);
	iommu_unmap(client->domain, iova, len);
	pm_runtime_put_sync(mmu->dev);

	return 0;
}

static void msm_smmu_destroy(struct msm_mmu *mmu)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct platform_device *pdev = to_platform_device(smmu->client_dev);

	if (smmu->client_dev && smmu->client_dev_owned)
		platform_device_unregister(pdev);
	kfree(smmu);
}

struct device *msm_smmu_get_dev(struct msm_mmu *mmu)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);

	return smmu->client_dev;
}

static int msm_smmu_map_dma_buf(struct msm_mmu *mmu, struct sg_table *sgt,
		int dir, u32 flags)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);
	unsigned long attrs = 0x0;
	int ret;

#if defined(CONFIG_DISPLAY_SAMSUNG)
	int retry_cnt;
#endif

	if (!sgt || !client) {
		DRM_ERROR("sg table is invalid\n");
		return -ENOMEM;
	}

	/*
	 * For import buffer type, dma_map_sg_attrs is called during
	 * dma_buf_map_attachment and is not required to call again
	 */
	if (!(flags & MSM_BO_EXTBUF)) {
		ret = dma_map_sg_attrs(client->dev, sgt->sgl, sgt->nents, dir,
				attrs);

#if defined(CONFIG_DISPLAY_SAMSUNG)
		if (!in_interrupt()) {
			if (!ret) {
				for (retry_cnt = 0; retry_cnt < 62 ; retry_cnt++) {
					/* To wait free page by memory reclaim*/
					usleep_range(16000, 16000);

					pr_err("dma map sg failed : retry (%d)\n", retry_cnt);
					ret = dma_map_sg_attrs(client->dev, sgt->sgl, sgt->nents, dir,
						attrs);
					if (!ret)
						break;
				}
			}
		}
#endif

		if (!ret) {
			DRM_ERROR("dma map sg failed\n");
			return -ENOMEM;
		}
	}

	if (sgt && sgt->sgl) {
		DRM_DEBUG("%pad/0x%x/0x%x/0x%lx\n",
				&sgt->sgl->dma_address, sgt->sgl->dma_length,
				dir, attrs);
		SDE_EVT32(sgt->sgl->dma_address, sgt->sgl->dma_length,
				dir, attrs, client->secure);
	}

#if defined(CONFIG_DISPLAY_SAMSUNG) && defined(CONFIG_SEC_DEBUG)
	if (sec_debug_is_enabled() && sgt && sgt->sgl)
		ss_smmu_debug_map(SMMU_RT_DISPLAY_DEBUG, sgt);
#endif

	return 0;
}


static void msm_smmu_unmap_dma_buf(struct msm_mmu *mmu, struct sg_table *sgt,
		int dir, u32 flags)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);

	if (!sgt || !client) {
		DRM_ERROR("sg table is invalid\n");
		return;
	}

	if (sgt->sgl) {
		DRM_DEBUG("%pad/0x%x/0x%x\n",
				&sgt->sgl->dma_address, sgt->sgl->dma_length,
				dir);
		SDE_EVT32(sgt->sgl->dma_address, sgt->sgl->dma_length,
				dir, client->secure);
	}

#if defined(CONFIG_DISPLAY_SAMSUNG) && defined(CONFIG_SEC_DEBUG)
	if (sec_debug_is_enabled() && sgt && sgt->sgl)
		ss_smmu_debug_unmap(SMMU_RT_DISPLAY_DEBUG, sgt);
#endif

	if (!(flags & MSM_BO_EXTBUF))
		dma_unmap_sg(client->dev, sgt->sgl, sgt->nents, dir);
}

static bool msm_smmu_is_domain_secure(struct msm_mmu *mmu)
{
	struct msm_smmu *smmu = to_msm_smmu(mmu);
	struct msm_smmu_client *client = msm_smmu_to_client(smmu);

	return client->secure;
}

static const struct msm_mmu_funcs funcs = {
	.attach = msm_smmu_attach,
	.detach = msm_smmu_detach,
	.map = msm_smmu_map,
	.unmap = msm_smmu_unmap,
	.map_dma_buf = msm_smmu_map_dma_buf,
	.unmap_dma_buf = msm_smmu_unmap_dma_buf,
	.destroy = msm_smmu_destroy,
	.is_domain_secure = msm_smmu_is_domain_secure,
	.set_attribute = msm_smmu_set_attribute,
	.one_to_one_map = msm_smmu_one_to_one_map,
	.one_to_one_unmap = msm_smmu_one_to_one_unmap,
	.get_dev = msm_smmu_get_dev,
};

static struct msm_smmu_domain msm_smmu_domains[MSM_SMMU_DOMAIN_MAX] = {
	[MSM_SMMU_DOMAIN_UNSECURE] = {
		.label = "mdp_ns",
		.secure = false,
	},
	[MSM_SMMU_DOMAIN_SECURE] = {
		.label = "mdp_s",
		.secure = true,
	},
	[MSM_SMMU_DOMAIN_NRT_UNSECURE] = {
		.label = "rot_ns",
		.secure = false,
	},
	[MSM_SMMU_DOMAIN_NRT_SECURE] = {
		.label = "rot_s",
		.secure = true,
	},
};

static const struct of_device_id msm_smmu_dt_match[] = {
	{ .compatible = "qcom,smmu_sde_unsec",
		.data = &msm_smmu_domains[MSM_SMMU_DOMAIN_UNSECURE] },
	{ .compatible = "qcom,smmu_sde_sec",
		.data = &msm_smmu_domains[MSM_SMMU_DOMAIN_SECURE] },
	{ .compatible = "qcom,smmu_sde_nrt_unsec",
		.data = &msm_smmu_domains[MSM_SMMU_DOMAIN_NRT_UNSECURE] },
	{ .compatible = "qcom,smmu_sde_nrt_sec",
		.data = &msm_smmu_domains[MSM_SMMU_DOMAIN_NRT_SECURE] },
	{}
};
MODULE_DEVICE_TABLE(of, msm_smmu_dt_match);

static struct device *msm_smmu_device_create(struct device *dev,
		enum msm_mmu_domain_type domain,
		struct msm_smmu *smmu)
{
	struct device_node *child;
	struct platform_device *pdev;
	bool existing = false;
	int i;
	const char *compat = NULL;

	for (i = 0; i < ARRAY_SIZE(msm_smmu_dt_match); i++) {
		if (msm_smmu_dt_match[i].data == &msm_smmu_domains[domain]) {
			compat = msm_smmu_dt_match[i].compatible;
			break;
		}
	}

	if (!compat) {
		DRM_DEBUG("unable to find matching domain for %d\n", domain);
		return ERR_PTR(-ENOENT);
	}
	DRM_DEBUG("found domain %d compat: %s\n", domain, compat);

	a52_ackfr_record("SMMU create enter domain=%d compat=%s", domain, compat);
	child = of_find_compatible_node(dev->of_node, NULL, compat);
	if (!child) {
		a52_ackfr_record("SMMU create no-node domain=%d", domain);
		DRM_DEBUG("unable to find compatible node for %s\n", compat);
		return ERR_PTR(-ENODEV);
	}

	pdev = of_find_device_by_node(child);
	if (pdev) {
		existing = true;
	} else {
		pdev = of_platform_device_create(child, NULL, dev);
	}
	if (!pdev) {
		a52_ackfr_record("SMMU create no-pdev domain=%d", domain);
		DRM_ERROR("unable to create smmu platform dev for domain %d\n",
				domain);
		return ERR_PTR(-ENODEV);
	}

	smmu->client = platform_get_drvdata(pdev);
	a52_ackfr_record("SMMU create state domain=%d existing=%d driver=%d client=%d",
		domain, existing, !!pdev->dev.driver, !!smmu->client);
	if (!smmu->client) {
		a52_ackfr_record("SMMU create defer domain=%d existing=%d",
			domain, existing);
		if (existing)
			put_device(&pdev->dev);
		else {
			of_node_clear_flag(child, OF_POPULATED);
			platform_device_unregister(pdev);
		}
		return ERR_PTR(-EPROBE_DEFER);
	}

	smmu->client_dev_owned = !existing;
	a52_ackfr_record("SMMU create ready domain=%d secure=%d owned=%d", domain,
		smmu->client->secure, smmu->client_dev_owned);
	if (existing)
		put_device(&pdev->dev);
	return &pdev->dev;
}

struct msm_mmu *msm_smmu_new(struct device *dev,
		enum msm_mmu_domain_type domain)
{
	struct msm_smmu *smmu;
	struct device *client_dev;

	/* Secure and non-secure display domains now share the validated
	 * ARM SMMU path. Secure page tables are assigned using qcom,iommu-vmid.
	 */

	smmu = kzalloc(sizeof(*smmu), GFP_KERNEL);
	if (!smmu)
		return ERR_PTR(-ENOMEM);

	client_dev = msm_smmu_device_create(dev, domain, smmu);
	if (IS_ERR(client_dev)) {
		kfree(smmu);
		return (void *)client_dev ? : ERR_PTR(-ENODEV);
	}

	smmu->client_dev = client_dev;
	msm_mmu_init(&smmu->base, dev, &funcs);

	return &smmu->base;
}

static int msm_smmu_fault_handler(struct iommu_domain *domain,
		struct device *dev, unsigned long iova,
		int flags, void *token)
{
	struct msm_smmu_client *client;
	int rc = -EINVAL;

	if (!token) {
		DRM_ERROR("Error: token is NULL\n");
		return -EINVAL;
	}

	client = (struct msm_smmu_client *)token;

	/* see iommu.h for fault flags definition */
	SDE_EVT32(iova, flags);
	pr_emerg("A52P411 MSM_SMMU_FAULT dev=%s domain=%px iova=%lx flags=%x\n",
		client && client->dev ? dev_name(client->dev) : "none",
		domain, iova, flags);
	DRM_ERROR("trigger dump, iova=0x%08lx, flags=0x%x\n", iova, flags);
	DRM_ERROR("SMMU device:%s", client->dev ? client->dev->kobj.name : "");

	/*
	 * return -ENOSYS to allow smmu driver to dump out useful
	 * debug info.
	 */

#if defined(CONFIG_DISPLAY_SAMSUNG) && defined(CONFIG_SEC_DEBUG)
	ss_smmu_debug_log();
	SDE_DBG_DUMP("all", "dbg_bus", "vbif_dbg_bus", "panic"); // case 03250922
#endif

	return rc;
}


static int msm_smmu_component_bind(struct device *dev,
		struct device *master, void *data)
{
	a52_ackfr_record("SMMU component-bind dev=%s master=%s",
		dev_name(dev), dev_name(master));
	return 0;
}

static void msm_smmu_component_unbind(struct device *dev,
		struct device *master, void *data)
{
	a52_ackfr_record("SMMU component-unbind dev=%s master=%s",
		dev_name(dev), dev_name(master));
}

static const struct component_ops msm_smmu_component_ops = {
	.bind = msm_smmu_component_bind,
	.unbind = msm_smmu_component_unbind,
};

/**
 * msm_smmu_probe()
 * @pdev: platform device
 *
 * Each smmu context acts as a separate device and the context banks are
 * configured with a VA range.
 * Registers the clks as each context bank has its own clks, for which voting
 * has to be done everytime before using that context bank.
 */
static int msm_smmu_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct msm_smmu_client *client;
	const struct msm_smmu_domain *domain;
	int ret;

	match = of_match_device(msm_smmu_dt_match, &pdev->dev);
	a52_ackfr_record("SMMU probe enter match=%d driver=%d", !!match,
		!!pdev->dev.driver);
	if (!match || !match->data) {
		dev_err(&pdev->dev, "probe failed as match data is invalid\n");
		return -EINVAL;
	}

	domain = match->data;
	if (!domain) {
		dev_err(&pdev->dev, "no matching device found\n");
		return -EINVAL;
	}

	DRM_INFO("probing device %s\n", match->compatible);

	client = devm_kzalloc(&pdev->dev, sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	client->dev = &pdev->dev;
	client->domain = iommu_get_domain_for_dev(client->dev);
	a52_ackfr_record("SMMU probe domain compat=%s ready=%d",
		match->compatible, !!client->domain);
	if (!client->domain) {
		dev_err(&pdev->dev, "iommu get domain for dev deferred\n");
		a52_ackfr_record("SMMU probe defer compat=%s", match->compatible);
		return -EPROBE_DEFER;
	}
	client->secure = domain->secure;
	client->domain_attached = true;

	if (!client->dev->dma_parms)
		client->dev->dma_parms = devm_kzalloc(client->dev,
				sizeof(*client->dev->dma_parms), GFP_KERNEL);
	dma_set_max_seg_size(client->dev, ~0U);
	dma_set_seg_boundary(client->dev, ~0UL);

	iommu_set_fault_handler(client->domain,
			msm_smmu_fault_handler, (void *)client);
	pr_err("A52P411 SMMU_HANDLER installed dev=%s\n",
		client->dev ? dev_name(client->dev) : "none");

	DRM_INFO("Created domain %s, secure=%d\n",
			domain->label, domain->secure);

	platform_set_drvdata(pdev, client);
	a52_ackfr_record("SMMU probe ready compat=%s secure=%d",
		match->compatible, client->secure);

	a52_ackfr_record("SMMU component-add enter compat=%s",
		match->compatible);
	ret = component_add(&pdev->dev, &msm_smmu_component_ops);
	a52_ackfr_record("SMMU component-add exit rc=%d", ret);
	return ret;
}

static int msm_smmu_remove(struct platform_device *pdev)
{
	struct msm_smmu_client *client;

	component_del(&pdev->dev, &msm_smmu_component_ops);
	client = platform_get_drvdata(pdev);
	if (client)
		client->domain_attached = false;

	return 0;
}

static struct platform_driver msm_smmu_driver = {
	.probe = msm_smmu_probe,
	.remove = msm_smmu_remove,
	.driver = {
		.name = "msmdrm_smmu",
		.of_match_table = msm_smmu_dt_match,
		.suppress_bind_attrs = true,
	},
};

int __init msm_smmu_driver_init(void)
{
	int ret;

	a52_ackfr_record("SMMU driver-register enter");
	ret = platform_driver_register(&msm_smmu_driver);
	a52_ackfr_record("SMMU driver-register exit rc=%d", ret);
	if (ret)
		pr_err("mdss_smmu_register_driver() failed!\n");

	return ret;
}

void __exit msm_smmu_driver_cleanup(void)
{
	platform_driver_unregister(&msm_smmu_driver);
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MSM SMMU driver");

/* A52_PHASE411_FIFO_PANIC_SMMU_EXPERIMENT_V1: downstream display SMMU handler confirmation. */

/* A52_PHASE446Q_ION_PRIME_TRACE_V1: SMMU stage. */
