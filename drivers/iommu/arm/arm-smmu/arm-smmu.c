// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for ARM architected SMMU implementations.
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 *
 * This driver currently supports:
 *	- SMMUv1 and v2 implementations
 *	- Stream-matching and stream-indexing
 *	- v7/v8 long-descriptor format
 *	- Non-secure access to the SMMU
 *	- Context fault reporting
 *	- Extended Stream ID (16 bit)
 */

#define pr_fmt(fmt) "arm-smmu: " fmt

#include <linux/acpi.h>
#include <linux/acpi_iort.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-iommu.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_iommu.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/capability.h>
#include <linux/ratelimit.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/a52_ack_secure_flight_recorder.h>
#include "../../../../a52-compat/include/soc/qcom/secure_buffer.h"

#include <linux/amba/bus.h>
#include <linux/fsl/mc.h>

#include "arm-smmu.h"

/*
 * Apparently, some Qualcomm arm64 platforms which appear to expose their SMMU
 * global register space are still, in fact, using a hypervisor to mediate it
 * by trapping and emulating register accesses. Sadly, some deployed versions
 * of said trapping code have bugs wherein they go horribly wrong for stores
 * using r31 (i.e. XZR/WZR) as the source register.
 */
#define QCOM_DUMMY_VAL -1

#define MSI_IOVA_BASE			0x8000000
#define MSI_IOVA_LENGTH			0x100000

static int force_stage;
module_param(force_stage, int, S_IRUGO);
MODULE_PARM_DESC(force_stage,
	"Force SMMU mappings to be installed at a particular stage of translation. A value of '1' or '2' forces the corresponding stage. All other values are ignored (i.e. no stage is forced). Note that selecting a specific stage will disable support for nested translation.");
static bool disable_bypass =
	IS_ENABLED(CONFIG_ARM_SMMU_DISABLE_BYPASS_BY_DEFAULT);
module_param(disable_bypass, bool, S_IRUGO);
MODULE_PARM_DESC(disable_bypass,
	"Disable bypass streams such that incoming transactions from devices that are not attached to an iommu domain will report an abort back to the device and will not be allowed to pass through the SMMU.");

#define s2cr_init_val (struct arm_smmu_s2cr){				\
	.type = disable_bypass ? S2CR_TYPE_FAULT : S2CR_TYPE_BYPASS,	\
}

static bool using_legacy_binding, using_generic_binding;
/* A52_PHASE446T_KGSL_SMMU_RPM_SWITCH_V1
 * a52.kgsl_smmu_rpm=0 retains the Phase446R probe-time SMMU GDSC vote.
 * 1 (default) permits 446S RPM, with new Phase446T KGSL active-period hold.
 */
static bool a52_kgsl_smmu_rpm = true;
static int __init a52_kgsl_smmu_rpm_setup(char *arg)
{
    bool enabled;
    if (kstrtobool(arg, &enabled))
        return -EINVAL;
    a52_kgsl_smmu_rpm = enabled;
    pr_warn("P446T SMMU_RPM_CMDLINE enabled=%u\n", enabled);
    return 0;
}
early_param("a52.kgsl_smmu_rpm", a52_kgsl_smmu_rpm_setup);



/* A52_KGSL_SMMU_RPM_V1: A52 GPU SMMU only. No global PM change. */
static bool a52_kgsl_smmu_has_phase250_vdd(struct arm_smmu_device *smmu)
{
	struct device_node *np = smmu->dev->of_node;

	return smmu->a52_num_gdscs > 0 && np &&
		np->full_name && strstr(np->full_name, "smmu-kgsl@3d40000") &&
		of_property_read_bool(np, "vdd-supply") &&
		of_property_match_string(np, "qcom,regulator-names", "vdd") >= 0;
}

static void a52_kgsl_smmu_pm_diag(struct device *dev, const char *stage)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);

	pr_warn("P446S KGSL_SMMU_RPM stage=%s dev=%s domain=%px pm_enabled=%u usage=%d status=%d phase250_vdd=%u\n",
		stage, dev_name(dev), dev->pm_domain, pm_runtime_enabled(dev),
		atomic_read(&dev->power.usage_count), dev->power.runtime_status,
		smmu && a52_kgsl_smmu_has_phase250_vdd(smmu));
}



/* A52_PHASE250_GPU_SMMU_POWER_CONTRACT_V1
 * Qualcomm downstream SMMUv2 nodes may describe GDSCs as regulators via
 * qcom,regulator-names. Keep their lifetime paired with ACK's existing clock
 * lifetime: regulator on before clk_prepare/enable; regulator off after clocks.
 */
static const char a52_phase250_gpu_smmu_power_contract[] __used =
	"A52_PHASE250_GPU_SMMU_POWER_CONTRACT_V1";

static int a52_arm_smmu_init_gdscs(struct arm_smmu_device *smmu)
{
	struct device *dev = smmu->dev;
	int i, ret, count;

	count = of_property_count_strings(dev->of_node, "qcom,regulator-names");
	if (count <= 0) {
		smmu->a52_num_gdscs = 0;
		return 0;
	}

	smmu->a52_gdscs = devm_kcalloc(dev, count, sizeof(*smmu->a52_gdscs),
				       GFP_KERNEL);
	if (!smmu->a52_gdscs)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		const char *name;

		ret = of_property_read_string_index(dev->of_node,
				"qcom,regulator-names", i, &name);
		if (ret)
			return ret;
		smmu->a52_gdscs[i].supply = name;
	}

	ret = devm_regulator_bulk_get(dev, count, smmu->a52_gdscs);
	if (ret)
		return ret;

	smmu->a52_num_gdscs = count;
	return 0;
}

static int a52_arm_smmu_enable_gdscs(struct arm_smmu_device *smmu)
{
	if (!smmu->a52_num_gdscs)
		return 0;
	return regulator_bulk_enable(smmu->a52_num_gdscs, smmu->a52_gdscs);
}

static void a52_arm_smmu_disable_gdscs(struct arm_smmu_device *smmu)
{
	int ret;

	if (!smmu->a52_num_gdscs)
		return;
	ret = regulator_bulk_disable(smmu->a52_num_gdscs, smmu->a52_gdscs);
	if (ret)
		dev_warn(smmu->dev, "failed to disable SMMU regulators: %d\n", ret);
}

static bool a52_apps_smmu(const struct device *dev)
{
	return dev && dev->of_node &&
		of_device_is_compatible(dev->of_node, "qcom,qsmmu-v500");
}

static bool a52_apps_smmu_has_unmanaged_tbus(const struct device *dev)
{
	struct device_node *child;

	if (!a52_apps_smmu(dev))
		return false;

	for_each_available_child_of_node(dev->of_node, child) {
		if (of_device_is_compatible(child, "qcom,qsmmuv500-tbu")) {
			of_node_put(child);
			return true;
		}
	}

	return false;
}


static inline int arm_smmu_rpm_get(struct arm_smmu_device *smmu)
{
	if (pm_runtime_enabled(smmu->dev))
		return pm_runtime_resume_and_get(smmu->dev);

	return 0;
}

static inline void arm_smmu_rpm_put(struct arm_smmu_device *smmu)
{
	if (pm_runtime_enabled(smmu->dev))
		pm_runtime_put_autosuspend(smmu->dev);
}

static struct arm_smmu_domain *to_smmu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_domain, domain);
}

static struct arm_smmu_domain *cfg_to_smmu_domain(struct arm_smmu_cfg *cfg)
{
	return container_of(cfg, struct arm_smmu_domain, cfg);
}

struct arm_smmu_secure_pool_chunk {
	void *addr;
	size_t size;
	struct list_head list;
};

static bool arm_smmu_has_secure_vmid(struct arm_smmu_domain *smmu_domain)
{
	return smmu_domain->secure_vmid != VMID_INVAL;
}

static void arm_smmu_secure_domain_lock(
		struct arm_smmu_domain *smmu_domain)
{
	if (arm_smmu_has_secure_vmid(smmu_domain))
		mutex_lock(&smmu_domain->assign_lock);
}

static void arm_smmu_secure_domain_unlock(
		struct arm_smmu_domain *smmu_domain)
{
	if (arm_smmu_has_secure_vmid(smmu_domain))
		mutex_unlock(&smmu_domain->assign_lock);
}

static void *arm_smmu_secure_pool_remove(struct arm_smmu_domain *smmu_domain,
					 size_t size)
{
	struct arm_smmu_secure_pool_chunk *chunk;

	list_for_each_entry(chunk, &smmu_domain->secure_pool_list, list) {
		if (chunk->size == size) {
			void *addr = chunk->addr;

			list_del(&chunk->list);
			kfree(chunk);
			return addr;
		}
	}
	return NULL;
}

static int arm_smmu_secure_pool_add(struct arm_smmu_domain *smmu_domain,
				    void *addr, size_t size)
{
	struct arm_smmu_secure_pool_chunk *chunk;

	chunk = kmalloc(sizeof(*chunk), GFP_ATOMIC);
	if (!chunk)
		return -ENOMEM;
	chunk->addr = addr;
	chunk->size = size;
	memset(addr, 0, size);
	list_add(&chunk->list, &smmu_domain->secure_pool_list);
	return 0;
}

static int arm_smmu_prepare_pgtable(struct arm_smmu_domain *smmu_domain,
				    void *addr, size_t size)
{
	struct arm_smmu_pte_info *info;

	info = kzalloc(sizeof(*info), GFP_ATOMIC);
	if (!info)
		return -ENOMEM;
	info->virt_addr = addr;
	info->size = size;
	list_add_tail(&info->entry, &smmu_domain->pte_info_list);
	return 0;
}

static void arm_smmu_unprepare_pgtable(struct arm_smmu_domain *smmu_domain,
				       void *addr, size_t size)
{
	struct arm_smmu_pte_info *info;

	info = kzalloc(sizeof(*info), GFP_ATOMIC);
	if (!info)
		return;
	info->virt_addr = addr;
	info->size = size;
	list_add_tail(&info->entry, &smmu_domain->unassign_list);
}

static void *arm_smmu_alloc_pages_exact(void *cookie, size_t size, gfp_t gfp)
{
	struct arm_smmu_domain *smmu_domain = cookie;
	void *pages;

	if (!arm_smmu_has_secure_vmid(smmu_domain))
		return alloc_pages_exact(size, gfp);

	pages = arm_smmu_secure_pool_remove(smmu_domain, size);
	if (pages)
		return pages;

	pages = alloc_pages_exact(size, gfp);
	if (pages && arm_smmu_prepare_pgtable(smmu_domain, pages, size)) {
		free_pages_exact(pages, size);
		return NULL;
	}
	return pages;
}

static bool arm_smmu_forget_unassigned_pgtable(
		struct arm_smmu_domain *smmu_domain, void *addr);

static void arm_smmu_free_pages_exact(void *cookie, void *virt, size_t size)
{
	struct arm_smmu_domain *smmu_domain = cookie;

	if (!arm_smmu_has_secure_vmid(smmu_domain)) {
		free_pages_exact(virt, size);
		return;
	}
	if (arm_smmu_secure_pool_add(smmu_domain, virt, size)) {
		/* A page still on pte_info_list never left HLOS. Free it
		 * directly rather than issuing an invalid two-owner unassign.
		 */
		if (arm_smmu_forget_unassigned_pgtable(smmu_domain, virt))
			free_pages_exact(virt, size);
		else
			arm_smmu_unprepare_pgtable(smmu_domain, virt, size);
	}
}

static int arm_smmu_assign_table(struct arm_smmu_domain *smmu_domain)
{
	u32 source_vmid = VMID_HLOS;
	int dest_vmids[2] = { VMID_HLOS, smmu_domain->secure_vmid };
	int dest_perms[2] = { PERM_READ | PERM_WRITE, PERM_READ };
	struct arm_smmu_pte_info *info, *tmp;
	int ret = 0;

	if (!arm_smmu_has_secure_vmid(smmu_domain))
		return 0;

	list_for_each_entry_safe(info, tmp, &smmu_domain->pte_info_list, entry) {
		ret = hyp_assign_phys(virt_to_phys(info->virt_addr), info->size,
				      &source_vmid, 1, dest_vmids, dest_perms, 2);
		if (WARN_ON(ret))
			break;
		list_del(&info->entry);
		kfree(info);
	}
	return ret;
}

static int arm_smmu_unassign_pages(struct arm_smmu_domain *smmu_domain,
				   void *addr, size_t size)
{
	u32 source_vmids[2] = { VMID_HLOS, smmu_domain->secure_vmid };
	int dest_vmid = VMID_HLOS;
	int dest_perm = PERM_READ | PERM_WRITE | PERM_EXEC;

	return hyp_assign_phys(virt_to_phys(addr), size, source_vmids, 2,
			       &dest_vmid, &dest_perm, 1);
}

static void arm_smmu_unassign_table(struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_pte_info *info, *tmp;
	int ret;

	if (!arm_smmu_has_secure_vmid(smmu_domain))
		return;

	list_for_each_entry_safe(info, tmp, &smmu_domain->unassign_list, entry) {
		ret = arm_smmu_unassign_pages(smmu_domain, info->virt_addr,
					      info->size);
		if (WARN_ON(ret))
			break;
		free_pages_exact(info->virt_addr, info->size);
		list_del(&info->entry);
		kfree(info);
	}
}

static bool arm_smmu_forget_unassigned_pgtable(
		struct arm_smmu_domain *smmu_domain, void *addr)
{
	struct arm_smmu_pte_info *info, *tmp;

	list_for_each_entry_safe(info, tmp, &smmu_domain->pte_info_list, entry) {
		if (info->virt_addr == addr) {
			list_del(&info->entry);
			kfree(info);
			return true;
		}
	}
	return false;
}

static void arm_smmu_secure_pool_destroy(struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_secure_pool_chunk *chunk, *tmp;
	int ret;

	list_for_each_entry_safe(chunk, tmp, &smmu_domain->secure_pool_list, list) {
		if (arm_smmu_forget_unassigned_pgtable(smmu_domain, chunk->addr)) {
			free_pages_exact(chunk->addr, chunk->size);
		} else {
			/* Pool destruction runs outside the io-pgtable allocator.
			 * Return ownership directly instead of allocating deferred
			 * unassign metadata which could fail during teardown.
			 */
			ret = arm_smmu_unassign_pages(smmu_domain, chunk->addr,
						      chunk->size);
			if (!WARN_ON(ret))
				free_pages_exact(chunk->addr, chunk->size);
		}
		list_del(&chunk->list);
		kfree(chunk);
	}
}

static struct platform_driver arm_smmu_driver;
static struct iommu_ops arm_smmu_ops;

#ifdef CONFIG_ARM_SMMU_LEGACY_DT_BINDINGS
static int arm_smmu_bus_init(struct iommu_ops *ops);

static struct device_node *dev_get_dev_node(struct device *dev)
{
	if (dev_is_pci(dev)) {
		struct pci_bus *bus = to_pci_dev(dev)->bus;

		while (!pci_is_root_bus(bus))
			bus = bus->parent;
		return of_node_get(bus->bridge->parent->of_node);
	}

	return of_node_get(dev->of_node);
}

static int __arm_smmu_get_pci_sid(struct pci_dev *pdev, u16 alias, void *data)
{
	*((__be32 *)data) = cpu_to_be32(alias);
	return 0; /* Continue walking */
}

static int __find_legacy_master_phandle(struct device *dev, void *data)
{
	struct of_phandle_iterator *it = *(void **)data;
	struct device_node *np = it->node;
	int err;

	of_for_each_phandle(it, err, dev->of_node, "mmu-masters",
			    "#stream-id-cells", -1)
		if (it->node == np) {
			*(void **)data = dev;
			return 1;
		}
	it->node = np;
	return err == -ENOENT ? 0 : err;
}

static int arm_smmu_register_legacy_master(struct device *dev,
					   struct arm_smmu_device **smmu)
{
	struct device *smmu_dev;
	struct device_node *np;
	struct of_phandle_iterator it;
	void *data = &it;
	u32 *sids;
	__be32 pci_sid;
	int err;

	np = dev_get_dev_node(dev);
	if (!np || !of_find_property(np, "#stream-id-cells", NULL)) {
		of_node_put(np);
		return -ENODEV;
	}

	it.node = np;
	err = driver_for_each_device(&arm_smmu_driver.driver, NULL, &data,
				     __find_legacy_master_phandle);
	smmu_dev = data;
	of_node_put(np);
	if (err == 0)
		return -ENODEV;
	if (err < 0)
		return err;

	if (dev_is_pci(dev)) {
		/* "mmu-masters" assumes Stream ID == Requester ID */
		pci_for_each_dma_alias(to_pci_dev(dev), __arm_smmu_get_pci_sid,
				       &pci_sid);
		it.cur = &pci_sid;
		it.cur_count = 1;
	}

	err = iommu_fwspec_init(dev, &smmu_dev->of_node->fwnode,
				&arm_smmu_ops);
	if (err)
		return err;

	sids = kcalloc(it.cur_count, sizeof(*sids), GFP_KERNEL);
	if (!sids)
		return -ENOMEM;

	*smmu = dev_get_drvdata(smmu_dev);
	of_phandle_iterator_args(&it, sids, it.cur_count);
	err = iommu_fwspec_add_ids(dev, sids, it.cur_count);
	kfree(sids);
	return err;
}

/*
 * With the legacy DT binding in play, we have no guarantees about
 * probe order, but then we're also not doing default domains, so we can
 * delay setting bus ops until we're sure every possible SMMU is ready,
 * and that way ensure that no probe_device() calls get missed.
 */
static int arm_smmu_legacy_bus_init(void)
{
	if (using_legacy_binding)
		return arm_smmu_bus_init(&arm_smmu_ops);
	return 0;
}
device_initcall_sync(arm_smmu_legacy_bus_init);
#else
static int arm_smmu_register_legacy_master(struct device *dev,
					   struct arm_smmu_device **smmu)
{
	return -ENODEV;
}
#endif /* CONFIG_ARM_SMMU_LEGACY_DT_BINDINGS */

static void __arm_smmu_free_bitmap(unsigned long *map, int idx)
{
	clear_bit(idx, map);
}

/* Wait for any pending TLB invalidations to complete */
static void __arm_smmu_tlb_sync(struct arm_smmu_device *smmu, int page,
				int sync, int status)
{
	unsigned int spin_cnt, delay;
	u32 reg;

	if (smmu->impl && unlikely(smmu->impl->tlb_sync))
		return smmu->impl->tlb_sync(smmu, page, sync, status);

	arm_smmu_writel(smmu, page, sync, QCOM_DUMMY_VAL);
	for (delay = 1; delay < TLB_LOOP_TIMEOUT; delay *= 2) {
		for (spin_cnt = TLB_SPIN_COUNT; spin_cnt > 0; spin_cnt--) {
			reg = arm_smmu_readl(smmu, page, status);
			if (!(reg & ARM_SMMU_sTLBGSTATUS_GSACTIVE))
				return;
			cpu_relax();
		}
		udelay(delay);
	}
	dev_err_ratelimited(smmu->dev,
			    "TLB sync timed out -- SMMU may be deadlocked\n");
}

static void arm_smmu_tlb_sync_global(struct arm_smmu_device *smmu)
{
	unsigned long flags;

	spin_lock_irqsave(&smmu->global_sync_lock, flags);
	__arm_smmu_tlb_sync(smmu, ARM_SMMU_GR0, ARM_SMMU_GR0_sTLBGSYNC,
			    ARM_SMMU_GR0_sTLBGSTATUS);
	spin_unlock_irqrestore(&smmu->global_sync_lock, flags);
}

static void arm_smmu_tlb_sync_context(struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	unsigned long flags;

	spin_lock_irqsave(&smmu_domain->cb_lock, flags);
	__arm_smmu_tlb_sync(smmu, ARM_SMMU_CB(smmu, smmu_domain->cfg.cbndx),
			    ARM_SMMU_CB_TLBSYNC, ARM_SMMU_CB_TLBSTATUS);
	spin_unlock_irqrestore(&smmu_domain->cb_lock, flags);
}

static void arm_smmu_tlb_inv_context_s1(void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;
	/*
	 * The TLBI write may be relaxed, so ensure that PTEs cleared by the
	 * current CPU are visible beforehand.
	 */
	wmb();
	arm_smmu_cb_write(smmu_domain->smmu, smmu_domain->cfg.cbndx,
			  ARM_SMMU_CB_S1_TLBIASID, smmu_domain->cfg.asid);
	arm_smmu_tlb_sync_context(smmu_domain);
}

static void arm_smmu_tlb_inv_context_s2(void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;
	struct arm_smmu_device *smmu = smmu_domain->smmu;

	/* See above */
	wmb();
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIVMID, smmu_domain->cfg.vmid);
	arm_smmu_tlb_sync_global(smmu);
}

static void arm_smmu_tlb_inv_range_s1(unsigned long iova, size_t size,
				      size_t granule, void *cookie, int reg)
{
	struct arm_smmu_domain *smmu_domain = cookie;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	int idx = cfg->cbndx;

	if (smmu->features & ARM_SMMU_FEAT_COHERENT_WALK)
		wmb();

	if (cfg->fmt != ARM_SMMU_CTX_FMT_AARCH64) {
		iova = (iova >> 12) << 12;
		iova |= cfg->asid;
		do {
			arm_smmu_cb_write(smmu, idx, reg, iova);
			iova += granule;
		} while (size -= granule);
	} else {
		iova >>= 12;
		iova |= (u64)cfg->asid << 48;
		do {
			arm_smmu_cb_writeq(smmu, idx, reg, iova);
			iova += granule >> 12;
		} while (size -= granule);
	}
}

static void arm_smmu_tlb_inv_range_s2(unsigned long iova, size_t size,
				      size_t granule, void *cookie, int reg)
{
	struct arm_smmu_domain *smmu_domain = cookie;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	int idx = smmu_domain->cfg.cbndx;

	if (smmu->features & ARM_SMMU_FEAT_COHERENT_WALK)
		wmb();

	iova >>= 12;
	do {
		if (smmu_domain->cfg.fmt == ARM_SMMU_CTX_FMT_AARCH64)
			arm_smmu_cb_writeq(smmu, idx, reg, iova);
		else
			arm_smmu_cb_write(smmu, idx, reg, iova);
		iova += granule >> 12;
	} while (size -= granule);
}

static void arm_smmu_tlb_inv_walk_s1(unsigned long iova, size_t size,
				     size_t granule, void *cookie)
{
	arm_smmu_tlb_inv_range_s1(iova, size, granule, cookie,
				  ARM_SMMU_CB_S1_TLBIVA);
	arm_smmu_tlb_sync_context(cookie);
}

static void arm_smmu_tlb_add_page_s1(struct iommu_iotlb_gather *gather,
				     unsigned long iova, size_t granule,
				     void *cookie)
{
	arm_smmu_tlb_inv_range_s1(iova, granule, granule, cookie,
				  ARM_SMMU_CB_S1_TLBIVAL);
}

static void arm_smmu_tlb_inv_walk_s2(unsigned long iova, size_t size,
				     size_t granule, void *cookie)
{
	arm_smmu_tlb_inv_range_s2(iova, size, granule, cookie,
				  ARM_SMMU_CB_S2_TLBIIPAS2);
	arm_smmu_tlb_sync_context(cookie);
}

static void arm_smmu_tlb_add_page_s2(struct iommu_iotlb_gather *gather,
				     unsigned long iova, size_t granule,
				     void *cookie)
{
	arm_smmu_tlb_inv_range_s2(iova, granule, granule, cookie,
				  ARM_SMMU_CB_S2_TLBIIPAS2L);
}

static void arm_smmu_tlb_inv_walk_s2_v1(unsigned long iova, size_t size,
					size_t granule, void *cookie)
{
	arm_smmu_tlb_inv_context_s2(cookie);
}
/*
 * On MMU-401 at least, the cost of firing off multiple TLBIVMIDs appears
 * almost negligible, but the benefit of getting the first one in as far ahead
 * of the sync as possible is significant, hence we don't just make this a
 * no-op and call arm_smmu_tlb_inv_context_s2() from .iotlb_sync as you might
 * think.
 */
static void arm_smmu_tlb_add_page_s2_v1(struct iommu_iotlb_gather *gather,
					unsigned long iova, size_t granule,
					void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;
	struct arm_smmu_device *smmu = smmu_domain->smmu;

	if (smmu->features & ARM_SMMU_FEAT_COHERENT_WALK)
		wmb();

	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIVMID, smmu_domain->cfg.vmid);
}

static const struct iommu_flush_ops arm_smmu_s1_tlb_ops = {
	.tlb_flush_all	= arm_smmu_tlb_inv_context_s1,
	.tlb_flush_walk	= arm_smmu_tlb_inv_walk_s1,
	.tlb_add_page	= arm_smmu_tlb_add_page_s1,
};

static const struct iommu_flush_ops arm_smmu_s2_tlb_ops_v2 = {
	.tlb_flush_all	= arm_smmu_tlb_inv_context_s2,
	.tlb_flush_walk	= arm_smmu_tlb_inv_walk_s2,
	.tlb_add_page	= arm_smmu_tlb_add_page_s2,
};

static const struct iommu_flush_ops arm_smmu_s2_tlb_ops_v1 = {
	.tlb_flush_all	= arm_smmu_tlb_inv_context_s2,
	.tlb_flush_walk	= arm_smmu_tlb_inv_walk_s2_v1,
	.tlb_add_page	= arm_smmu_tlb_add_page_s2_v1,
};


/* A52_PHASE422_DISPLAY_SMMU_FAULT_PROBE_V1
 * Cache SID 0x800's exact domain/context IRQ registration during SMMU setup,
 * but defer persistent writes until the Phase421 target is active and B19/B1A
 * survival RAM is known to be initialized.
 */
extern bool a52_p421_target_active(void);
extern void a52_p422_survival_record_at(u32 stage, u32 seq, u64 ts_ns,
					 u64 arg0, u64 arg1,
					 u64 arg2, u64 arg3);

struct a52_p422_irq_cache {
	struct iommu_domain *domain;
	struct arm_smmu_device *smmu;
	u16 sid;
	u8 cbndx;
	u8 irptndx;
	int irq;
	int request_ret;
	bool valid;
};

static struct a52_p422_irq_cache a52_p422_irq_cache;
static atomic_t a52_p422_fault_count = ATOMIC_INIT(0);

static bool a52_p422_dev_has_sid800(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	int i;

	if (!fwspec)
		return false;
	for (i = 0; i < fwspec->num_ids; i++) {
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);

		if (sid == 0x800)
			return true;
	}
	return false;
}

static bool a52_p422_target_domain(struct iommu_domain *domain, int cbndx)
{
	return a52_p421_target_active() &&
	       READ_ONCE(a52_p422_irq_cache.valid) &&
	       a52_p422_irq_cache.sid == 0x800 &&
	       a52_p422_irq_cache.domain == domain &&
	       a52_p422_irq_cache.cbndx == cbndx;
}

void a52_p422_publish_m00(u64 cmd_buffer_iova)
{
	struct a52_p422_irq_cache *c = &a52_p422_irq_cache;
	u64 packed;

	if (!READ_ONCE(c->valid) || c->sid != 0x800)
		return;

	packed = ((u64)c->sid << 48) |
		 ((u64)c->cbndx << 40) |
		 ((u64)c->irptndx << 32) |
		 (u32)c->irq;
	a52_p422_survival_record_at(0U, 1U, ktime_get_boottime_ns(),
		cmd_buffer_iova, packed, (u64)(s64)c->request_ret, 0);
}
EXPORT_SYMBOL_GPL(a52_p422_publish_m00);

void a52_p422_display_timeout_snapshot(u64 cmd_buffer_iova)
{
	struct a52_p422_irq_cache *c = &a52_p422_irq_cache;
	u32 fsr = ~0U, sctlr = ~0U;
	u64 far = ~0ULL;
	u32 count;
	int ret;

	if (!a52_p422_target_domain(c->domain, c->cbndx) || !c->smmu)
		return;

	ret = arm_smmu_rpm_get(c->smmu);
	if (ret >= 0) {
		fsr = arm_smmu_cb_read(c->smmu, c->cbndx, ARM_SMMU_CB_FSR);
		far = arm_smmu_cb_readq(c->smmu, c->cbndx, ARM_SMMU_CB_FAR);
		sctlr = arm_smmu_cb_read(c->smmu, c->cbndx, ARM_SMMU_CB_SCTLR);
		arm_smmu_rpm_put(c->smmu);
	}
	count = (u32)atomic_read(&a52_p422_fault_count);
	a52_p422_survival_record_at(2U, count, ktime_get_boottime_ns(),
		cmd_buffer_iova, far, ((u64)sctlr << 32) | fsr,
		((u64)(u32)ret << 32) | count);
}
EXPORT_SYMBOL_GPL(a52_p422_display_timeout_snapshot);


/* A52_PHASE426_SMMU_STREAM_HANDOFF_PROBE_V1
 *
 * Read-only diagnosis of the qcom,skip-init firmware handoff contract.
 *
 * Phase68 deliberately preserved bootloader SMR/S2CR/context-bank hardware,
 * but did not import those routes into Linux smrs[]/s2crs[]/context_map.
 * TouchGrass does import them in arm_smmu_handoff_cbs().
 *
 * This phase DOES NOT repair that difference. It only:
 *   - snapshots every firmware-valid SMR/S2CR before reset/test writes;
 *   - observes which SMR arm_smmu_test_smr_masks() uses and its before/after;
 *   - snapshots HW + Linux SW routes at exact F0 S00;
 *   - latches (without recorder I/O) every apps-SMMU global fault;
 *   - after S10/timeout, emits the evidence to the existing persistent recorder.
 *
 * No SMR/S2CR/CB/global-fault register is written by Phase426.
 */
#define A52_P426_MAX_SMRS 128U
#define A52_P426_SID      0x0800U

struct a52_p426_hw_route {
	u32 smr;
	u32 s2cr;
};

struct a52_p426_sw_route {
	u16 id;
	u16 mask;
	u8 cbndx;
	u8 type;
	u8 valid;
	u8 pinned;
	s32 count;
};

struct a52_p426_snapshot {
	struct a52_p426_hw_route hw[A52_P426_MAX_SMRS];
	struct a52_p426_sw_route sw[A52_P426_MAX_SMRS];
	u32 groups;
	u32 hw_valid;
	u32 hw_match;
	u32 sw_valid;
	u32 sw_match;
	u32 gfsr;
	u32 gfsynr0;
	u32 gfsynr1;
	u32 gfsynr2;
};

static struct arm_smmu_device *a52_p426_apps_smmu;
static struct a52_p426_snapshot a52_p426_boot;
static struct a52_p426_snapshot a52_p426_f0;
static struct a52_p426_snapshot a52_p426_timeout;
static atomic_t a52_p426_boot_ready = ATOMIC_INIT(0);
static atomic_t a52_p426_f0_taken = ATOMIC_INIT(0);
static atomic_t a52_p426_global_faults = ATOMIC_INIT(0);
static int a52_p426_mask_test_idx = -1;
static u32 a52_p426_mask_test_before;
static u32 a52_p426_mask_test_after;
static u32 a52_p426_last_gfsr;
static u32 a52_p426_last_gfsynr0;
static u32 a52_p426_last_gfsynr1;
static u32 a52_p426_last_gfsynr2;
static u32 a52_p426_p422_valid;
static u32 a52_p426_p422_sid;
static u32 a52_p426_p422_same;
static u32 a52_p426_p422_cbndx;

static bool a52_p426_is_target(struct arm_smmu_device *smmu)
{
	/* The preserved A52 Phase424 DTB has exactly one qcom,qsmmu-v500 +
	 * qcom,skip-init instance: /soc/apps-smmu@15000000. Keep the probe
	 * explicitly scoped to that node rather than relying on compatibility
	 * alone.
	 */
	return smmu && smmu->skip_init && smmu->dev && smmu->dev->of_node &&
	       of_device_is_compatible(smmu->dev->of_node, "qcom,qsmmu-v500") &&
	       of_node_name_eq(smmu->dev->of_node, "apps-smmu");
}

static bool a52_p426_check_p422(struct arm_smmu_device *smmu)
{
	struct arm_smmu_device *p422_smmu;
	u16 sid;
	u8 cbndx;
	bool valid;
	bool same;

	valid = READ_ONCE(a52_p422_irq_cache.valid);
	if (!valid) {
		WRITE_ONCE(a52_p426_p422_valid, 0U);
		return true;
	}

	p422_smmu = READ_ONCE(a52_p422_irq_cache.smmu);
	sid = READ_ONCE(a52_p422_irq_cache.sid);
	cbndx = READ_ONCE(a52_p422_irq_cache.cbndx);
	same = p422_smmu == smmu;

	WRITE_ONCE(a52_p426_p422_valid, 1U);
	WRITE_ONCE(a52_p426_p422_sid, (u32)sid);
	WRITE_ONCE(a52_p426_p422_cbndx, (u32)cbndx);
	WRITE_ONCE(a52_p426_p422_same, same ? 1U : 0U);
	smp_wmb();

	return sid == A52_P426_SID && same;
}

static bool a52_p426_hw_valid(struct arm_smmu_device *smmu, u32 smr, u32 s2cr)
{
	if (smmu->features & ARM_SMMU_FEAT_EXIDS)
		return !!(s2cr & ARM_SMMU_S2CR_EXIDVALID);
	return !!(smr & ARM_SMMU_SMR_VALID);
}

static u16 a52_p426_decode_mask(struct arm_smmu_device *smmu, u32 smr)
{
	u16 mask = (u16)FIELD_GET(ARM_SMMU_SMR_MASK, smr);

	/* In non-EXIDS mode SMR bit31 is VALID, although the generic 5.10
	 * ARM_SMMU_SMR_MASK macro spans bits31:16. With EXIDS, validity moves
	 * to S2CR.EXIDVALID and bit31 is a real mask bit.
	 */
	if (!(smmu->features & ARM_SMMU_FEAT_EXIDS))
		mask &= 0x7fffU;
	return mask;
}

static bool a52_p426_sid_match_raw(struct arm_smmu_device *smmu,
				    u32 smr, u32 s2cr, u16 sid)
{
	u16 id;
	u16 mask;

	if (!a52_p426_hw_valid(smmu, smr, s2cr))
		return false;

	id = (u16)FIELD_GET(ARM_SMMU_SMR_ID, smr);
	mask = a52_p426_decode_mask(smmu, smr);
	return !((sid ^ id) & (u16)~mask);
}

static bool a52_p426_sid_match_sw(const struct arm_smmu_smr *smr, u16 sid)
{
	if (!smr || !smr->valid)
		return false;
	return !((sid ^ smr->id) & (u16)~smr->mask);
}

static void a52_p426_take_snapshot(struct arm_smmu_device *smmu,
				   struct a52_p426_snapshot *snap,
				   bool include_sw)
{
	u32 i;
	u32 groups;

	if (!a52_p426_is_target(smmu) || !snap)
		return;

	memset(snap, 0, sizeof(*snap));
	groups = min_t(u32, smmu->num_mapping_groups, A52_P426_MAX_SMRS);
	snap->groups = groups;

	for (i = 0; i < groups; i++) {
		u32 raw_smr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
		u32 raw_s2cr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_S2CR(i));

		snap->hw[i].smr = raw_smr;
		snap->hw[i].s2cr = raw_s2cr;
		if (a52_p426_hw_valid(smmu, raw_smr, raw_s2cr))
			snap->hw_valid++;
		if (a52_p426_sid_match_raw(smmu, raw_smr, raw_s2cr,
					    A52_P426_SID))
			snap->hw_match++;

		if (include_sw && smmu->smrs && smmu->s2crs) {
			struct a52_p426_sw_route *d = &snap->sw[i];
			struct arm_smmu_smr *smr = &smmu->smrs[i];
			struct arm_smmu_s2cr *s2cr = &smmu->s2crs[i];

			d->id = smr->id;
			d->mask = smr->mask;
			d->cbndx = s2cr->cbndx;
			d->type = (u8)s2cr->type;
			d->valid = smr->valid ? 1U : 0U;
			d->pinned = s2cr->pinned ? 1U : 0U;
			d->count = s2cr->count;
			if (smr->valid)
				snap->sw_valid++;
			if (a52_p426_sid_match_sw(smr, A52_P426_SID))
				snap->sw_match++;
		}
	}

	snap->gfsr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSR);
	snap->gfsynr0 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR0);
	snap->gfsynr1 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR1);
	snap->gfsynr2 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR2);
}

static void a52_p446_emit_smmu(struct arm_smmu_device *smmu,u32 event,int sme,u32 cb,u32 aux);
/* A52_PHASE446L_CB2_LIVE_V1: record CB2 and raw route SMR3/S2CR3.
 * Caller must not issue any SMMU power transition in the hot path.
 */
void a52_p446l_cb2_live(u32 *route,u32 *fsr,u64 *far,
                        u32 *fsynr0,u32 *sctlr,u64 *ttbr0)
{
    struct arm_smmu_device *s=READ_ONCE(a52_p426_apps_smmu);
    if (!s || !s->dev || !pm_runtime_active(s->dev) ||
        s->num_context_banks <= 2U)
        return;
    *route=arm_smmu_gr0_read(s,ARM_SMMU_GR0_S2CR(3));
    *fsr=arm_smmu_cb_read(s,2,ARM_SMMU_CB_FSR);
    *far=arm_smmu_cb_readq(s,2,ARM_SMMU_CB_FAR);
    *fsynr0=arm_smmu_cb_read(s,2,ARM_SMMU_CB_FSYNR0);
    *sctlr=arm_smmu_cb_read(s,2,ARM_SMMU_CB_SCTLR);
    *ttbr0=arm_smmu_cb_readq(s,2,ARM_SMMU_CB_TTBR0);
}
EXPORT_SYMBOL_GPL(a52_p446l_cb2_live);

static void a52_p426_cache_boot_routes(struct arm_smmu_device *smmu)
{
	if (!a52_p426_is_target(smmu))
		return;

	WRITE_ONCE(a52_p426_apps_smmu, smmu);
	/* arm_smmu_device_cfg_probe() has allocated/initialized Linux's software
	 * tables, while qcom,skip-init hardware still contains firmware state.
	 * Capture immediately after cfg_probe succeeds, before iommu_device_register()
	 * can expose the provider to client probes, and before reset/mask-test.
	 */
	a52_p426_take_snapshot(smmu, &a52_p426_boot, true);
	{ int j; for(j=0;j<smmu->num_mapping_groups;j++){ u32 sr=arm_smmu_gr0_read(smmu,ARM_SMMU_GR0_SMR(j)); u32 s2=arm_smmu_gr0_read(smmu,ARM_SMMU_GR0_S2CR(j)); if(a52_p426_sid_match_raw(smmu,sr,s2,A52_P426_SID)){ u32 cb=FIELD_GET(ARM_SMMU_S2CR_CBNDX,s2); a52_p446_emit_smmu(smmu,1U,j,cb,0U); break; } } }
	smp_wmb();
	atomic_set(&a52_p426_boot_ready, 1);
}

void a52_p426_capture_f0(void)
{
	struct arm_smmu_device *smmu = READ_ONCE(a52_p426_apps_smmu);

	if (!smmu || !atomic_read(&a52_p426_boot_ready))
		return;
	if (atomic_cmpxchg(&a52_p426_f0_taken, 0, 1) != 0)
		return;
	if (!a52_p426_check_p422(smmu))
		return;

	/* Exact S00 observer: reads + normal-RAM stores only. No printk,
	 * recorder I/O, persistence, delays, votes, or SMMU writes.
	 */
	a52_p426_take_snapshot(smmu, &a52_p426_f0, true);
	smp_wmb();
}
EXPORT_SYMBOL_GPL(a52_p426_capture_f0);

static void a52_p426_latch_global_fault(struct arm_smmu_device *smmu,
					u32 gfsr, u32 s0, u32 s1, u32 s2)
{
	if (!a52_p426_is_target(smmu) || !gfsr)
		return;

	WRITE_ONCE(a52_p426_last_gfsr, gfsr);
	WRITE_ONCE(a52_p426_last_gfsynr0, s0);
	WRITE_ONCE(a52_p426_last_gfsynr1, s1);
	WRITE_ONCE(a52_p426_last_gfsynr2, s2);
	smp_wmb();
	atomic_inc(&a52_p426_global_faults);
}

static void a52_p426_dump_hw_matches(const char *tag,
				      const struct a52_p426_snapshot *snap,
				      struct arm_smmu_device *smmu)
{
	u32 i;

	for (i = 0; i < snap->groups; i++) {
		u32 smr = snap->hw[i].smr;
		u32 s2cr = snap->hw[i].s2cr;

		if (!a52_p426_sid_match_raw(smmu, smr, s2cr, A52_P426_SID))
			continue;
		a52_ackfr_record("P426 %s i=%u smr=%x s2=%x id=%x m=%x cb=%u ty=%u",
			tag, i, smr, s2cr,
			(u32)(u16)FIELD_GET(ARM_SMMU_SMR_ID, smr),
			(u32)a52_p426_decode_mask(smmu, smr),
			(u32)FIELD_GET(ARM_SMMU_S2CR_CBNDX, s2cr),
			(u32)FIELD_GET(ARM_SMMU_S2CR_TYPE, s2cr));
	}
}

static void a52_p426_dump_sw_matches(const struct a52_p426_snapshot *snap)
{
	u32 i;

	for (i = 0; i < snap->groups; i++) {
		const struct a52_p426_sw_route *r = &snap->sw[i];

		if (!r->valid ||
		    ((A52_P426_SID ^ r->id) & (u16)~r->mask))
			continue;
		a52_ackfr_record("P426 SW i=%u id=%x m=%x cb=%u ty=%u c=%d pin=%u",
			i, (u32)r->id, (u32)r->mask, (u32)r->cbndx,
			(u32)r->type, r->count, (u32)r->pinned);
	}
}

void a52_p426_timeout_dump(void)
{
	struct arm_smmu_device *smmu = READ_ONCE(a52_p426_apps_smmu);
	u32 i;
	u32 gf;

	if (!smmu || !atomic_read(&a52_p426_boot_ready))
		return;

	(void)a52_p426_check_p422(smmu);
	a52_p426_take_snapshot(smmu, &a52_p426_timeout, true);
	gf = (u32)atomic_read(&a52_p426_global_faults);

	a52_ackfr_record("P426 X pv=%u sid=%x same=%u cb=%u",
		READ_ONCE(a52_p426_p422_valid),
		READ_ONCE(a52_p426_p422_sid),
		READ_ONCE(a52_p426_p422_same),
		READ_ONCE(a52_p426_p422_cbndx));
	a52_ackfr_record("P426 B g=%u hv=%u hm=%u sv=%u sm=%u",
		a52_p426_boot.groups, a52_p426_boot.hw_valid,
		a52_p426_boot.hw_match, a52_p426_boot.sw_valid,
		a52_p426_boot.sw_match);
	a52_ackfr_record("P426 MT i=%d pre=%x post=%x",
		a52_p426_mask_test_idx, a52_p426_mask_test_before,
		a52_p426_mask_test_after);
	a52_ackfr_record("P426 F g=%u hv=%u hm=%u sv=%u sm=%u",
		a52_p426_f0.groups, a52_p426_f0.hw_valid,
		a52_p426_f0.hw_match, a52_p426_f0.sw_valid,
		a52_p426_f0.sw_match);
	a52_ackfr_record("P426 T g=%u hv=%u hm=%u sv=%u sm=%u",
		a52_p426_timeout.groups, a52_p426_timeout.hw_valid,
		a52_p426_timeout.hw_match, a52_p426_timeout.sw_valid,
		a52_p426_timeout.sw_match);
	a52_ackfr_record("P426 GF n=%u g=%x s0=%x s1=%x s2=%x",
		gf, READ_ONCE(a52_p426_last_gfsr),
		READ_ONCE(a52_p426_last_gfsynr0),
		READ_ONCE(a52_p426_last_gfsynr1),
		READ_ONCE(a52_p426_last_gfsynr2));
	a52_ackfr_record("P426 TG g=%x s0=%x s1=%x s2=%x",
		a52_p426_timeout.gfsr, a52_p426_timeout.gfsynr0,
		a52_p426_timeout.gfsynr1, a52_p426_timeout.gfsynr2);

	a52_p426_dump_hw_matches("BM", &a52_p426_boot, smmu);
	a52_p426_dump_hw_matches("FM", &a52_p426_f0, smmu);
	a52_p426_dump_hw_matches("TM", &a52_p426_timeout, smmu);
	a52_p426_dump_sw_matches(&a52_p426_f0);

	/* Full bootloader-valid table is emitted only after the failed transfer,
	 * never on the F0/pre-trigger path.
	 */
	for (i = 0; i < a52_p426_boot.groups; i++) {
		u32 smr = a52_p426_boot.hw[i].smr;
		u32 s2cr = a52_p426_boot.hw[i].s2cr;

		if (!a52_p426_hw_valid(smmu, smr, s2cr))
			continue;
		a52_ackfr_record("P426 BT i=%u smr=%x s2=%x", i, smr, s2cr);
	}
}
EXPORT_SYMBOL_GPL(a52_p426_timeout_dump);

static irqreturn_t arm_smmu_context_fault(int irq, void *dev)
{
	u32 fsr, fsynr, cbfrsynra;
	u32 a52_p422_sctlr;
	u64 a52_p422_ts;
	u32 a52_p422_count;
	unsigned long iova;
	struct iommu_domain *domain = dev;
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	int idx = smmu_domain->cfg.cbndx;

	if (unlikely(a52_kgsl_smmu_has_phase250_vdd(smmu) &&
		     pm_runtime_suspended(smmu->dev)))
		dev_warn_ratelimited(smmu->dev,
			"P446S KGSL_SMMU_RPM FAULT_IRQ context while suspended irq=%d\n",
			irq);

	fsr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSR);
	if (!(fsr & ARM_SMMU_FSR_FAULT))
		return IRQ_NONE;

	fsynr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSYNR0);
	iova = arm_smmu_cb_readq(smmu, idx, ARM_SMMU_CB_FAR);
	cbfrsynra = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBFRSYNRA(idx));

	/* Phase422: one extra context-bank read only when the exact SID 0x800
	 * Phase421 target is active. Record before the existing FSR clear.
	 */
	if (a52_p422_target_domain(domain, idx)) {
		a52_p422_ts = ktime_get_boottime_ns();
		a52_p422_sctlr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_SCTLR);
		a52_p422_count = (u32)atomic_inc_return(&a52_p422_fault_count);
		a52_p422_survival_record_at(1U, a52_p422_count, a52_p422_ts,
			(u64)iova, ((u64)fsynr << 32) | fsr,
			((u64)(u32)idx << 32) | a52_p422_sctlr,
			((u64)(u32)irq << 32) | cbfrsynra);
	}

	/* Phase411: values are already required by the real fault handler.
	 * Do not add any extra SMMU/MDSS MMIO reads here.
	 */
	pr_emerg("A52P411 SMMU_CTX irq=%d cb=%d fsr=%x syn=%x iova=%lx cbfr=%x\n",
		irq, idx, fsr, fsynr, iova, cbfrsynra);

	a52_ackfr_record("M393 C irq=%d cb=%d fsr=%x syn=%x iova=%lx",
		irq, idx, fsr, fsynr, iova);

	dev_err_ratelimited(smmu->dev,
	"Unhandled context fault: fsr=0x%x, iova=0x%08lx, fsynr=0x%x, cbfrsynra=0x%x, cb=%d\n",
			    fsr, iova, fsynr, cbfrsynra, idx);

	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_FSR, fsr);
	return IRQ_HANDLED;
}

static irqreturn_t arm_smmu_global_fault(int irq, void *dev)
{
	u32 gfsr, gfsynr0, gfsynr1, gfsynr2;
	struct arm_smmu_device *smmu = dev;
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);


	if (unlikely(a52_kgsl_smmu_has_phase250_vdd(smmu) &&
		     pm_runtime_suspended(smmu->dev)))
		dev_warn_ratelimited(smmu->dev,
			"P446S KGSL_SMMU_RPM FAULT_IRQ global while suspended irq=%d\n",
			irq);

	gfsr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSR);
	gfsynr0 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR0);
	gfsynr1 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR1);
	gfsynr2 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR2);

	a52_p426_latch_global_fault(smmu, gfsr, gfsynr0, gfsynr1, gfsynr2);

	if (!gfsr)
		return IRQ_NONE;

	a52_ackfr_record("M393 G irq=%d g=%x s0=%x s1=%x s2=%x",
		irq, gfsr, gfsynr0, gfsynr1, gfsynr2);

	if (__ratelimit(&rs)) {
		if (IS_ENABLED(CONFIG_ARM_SMMU_DISABLE_BYPASS_BY_DEFAULT) &&
		    (gfsr & ARM_SMMU_sGFSR_USF))
			dev_err(smmu->dev,
				"Blocked unknown Stream ID 0x%hx; boot with \"arm-smmu.disable_bypass=0\" to allow, but this may have security implications\n",
				(u16)gfsynr1);
		else
			dev_err(smmu->dev,
				"Unexpected global fault, this could be serious\n");
		dev_err(smmu->dev,
			"\tGFSR 0x%08x, GFSYNR0 0x%08x, GFSYNR1 0x%08x, GFSYNR2 0x%08x\n",
			gfsr, gfsynr0, gfsynr1, gfsynr2);
	}

	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sGFSR, gfsr);
	return IRQ_HANDLED;
}

static void arm_smmu_init_context_bank(struct arm_smmu_domain *smmu_domain,
				       struct io_pgtable_cfg *pgtbl_cfg)
{
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_cb *cb = &smmu_domain->smmu->cbs[cfg->cbndx];
	bool stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;

	cb->cfg = cfg;

	/* TCR */
	if (stage1) {
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S) {
			cb->tcr[0] = pgtbl_cfg->arm_v7s_cfg.tcr;
		} else {
			cb->tcr[0] = arm_smmu_lpae_tcr(pgtbl_cfg);
			cb->tcr[1] = arm_smmu_lpae_tcr2(pgtbl_cfg);
			if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)
				cb->tcr[1] |= ARM_SMMU_TCR2_AS;
			else
				cb->tcr[0] |= ARM_SMMU_TCR_EAE;
		}
	} else {
		cb->tcr[0] = arm_smmu_lpae_vtcr(pgtbl_cfg);
	}

	/* TTBRs */
	if (stage1) {
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S) {
			cb->ttbr[0] = pgtbl_cfg->arm_v7s_cfg.ttbr;
			cb->ttbr[1] = 0;
		} else {
			cb->ttbr[0] = FIELD_PREP(ARM_SMMU_TTBRn_ASID,
						 cfg->asid);
			cb->ttbr[1] = FIELD_PREP(ARM_SMMU_TTBRn_ASID,
						 cfg->asid);

			if (pgtbl_cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)
				cb->ttbr[1] |= pgtbl_cfg->arm_lpae_s1_cfg.ttbr;
			else
				cb->ttbr[0] |= pgtbl_cfg->arm_lpae_s1_cfg.ttbr;
		}
	} else {
		cb->ttbr[0] = pgtbl_cfg->arm_lpae_s2_cfg.vttbr;
	}

	/* MAIRs (stage-1 only) */
	if (stage1) {
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S) {
			cb->mair[0] = pgtbl_cfg->arm_v7s_cfg.prrr;
			cb->mair[1] = pgtbl_cfg->arm_v7s_cfg.nmrr;
		} else {
			cb->mair[0] = pgtbl_cfg->arm_lpae_s1_cfg.mair;
			cb->mair[1] = pgtbl_cfg->arm_lpae_s1_cfg.mair >> 32;
		}
	}
}

void arm_smmu_write_context_bank(struct arm_smmu_device *smmu, int idx)
{
	u32 reg;
	bool stage1;
	struct arm_smmu_cb *cb = &smmu->cbs[idx];
	struct arm_smmu_cfg *cfg = cb->cfg;

	/* Unassigned context banks only need disabling */
	if (!cfg) {
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, 0);
		return;
	}

	stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;

	/* CBA2R */
	if (smmu->version > ARM_SMMU_V1) {
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)
			reg = ARM_SMMU_CBA2R_VA64;
		else
			reg = 0;
		/* 16-bit VMIDs live in CBA2R */
		if (smmu->features & ARM_SMMU_FEAT_VMID16)
			reg |= FIELD_PREP(ARM_SMMU_CBA2R_VMID16, cfg->vmid);

		arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBA2R(idx), reg);
	}

	/* CBAR */
	reg = FIELD_PREP(ARM_SMMU_CBAR_TYPE, cfg->cbar);
	if (smmu->version < ARM_SMMU_V2)
		reg |= FIELD_PREP(ARM_SMMU_CBAR_IRPTNDX, cfg->irptndx);

	/*
	 * Use the weakest shareability/memory types, so they are
	 * overridden by the ttbcr/pte.
	 */
	if (stage1) {
		reg |= FIELD_PREP(ARM_SMMU_CBAR_S1_BPSHCFG,
				  ARM_SMMU_CBAR_S1_BPSHCFG_NSH) |
		       FIELD_PREP(ARM_SMMU_CBAR_S1_MEMATTR,
				  ARM_SMMU_CBAR_S1_MEMATTR_WB);
	} else if (!(smmu->features & ARM_SMMU_FEAT_VMID16)) {
		/* 8-bit VMIDs live in CBAR */
		reg |= FIELD_PREP(ARM_SMMU_CBAR_VMID, cfg->vmid);
	}
	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBAR(idx), reg);

	/*
	 * TCR
	 * We must write this before the TTBRs, since it determines the
	 * access behaviour of some fields (in particular, ASID[15:8]).
	 */
	if (stage1 && smmu->version > ARM_SMMU_V1)
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TCR2, cb->tcr[1]);
	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TCR, cb->tcr[0]);

	/* TTBRs */
	if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S) {
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_CONTEXTIDR, cfg->asid);
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TTBR0, cb->ttbr[0]);
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_TTBR1, cb->ttbr[1]);
	} else {
		arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_TTBR0, cb->ttbr[0]);
		if (stage1)
			arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_TTBR1,
					   cb->ttbr[1]);
	}

	/* MAIRs (stage-1 only) */
	if (stage1) {
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_S1_MAIR0, cb->mair[0]);
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_S1_MAIR1, cb->mair[1]);
	}

	/* SCTLR */
	reg = ARM_SMMU_SCTLR_CFIE | ARM_SMMU_SCTLR_CFRE | ARM_SMMU_SCTLR_AFE |
	      ARM_SMMU_SCTLR_TRE;
	/*
	 * TouchGrass leaves stage-1 translation disabled while the DT requests
	 * qcom,iommu-earlymap, preserving the live bootloader context until KMS
	 * has installed the splash mappings and explicitly clears the attribute.
	 */
	if (!stage1 || !(cfg_to_smmu_domain(cfg)->attributes &
			BIT(DOMAIN_ATTR_EARLY_MAP)))
		reg |= ARM_SMMU_SCTLR_M;
	if (stage1)
		reg |= ARM_SMMU_SCTLR_S1_ASIDPNE;
	if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN))
		reg |= ARM_SMMU_SCTLR_E;

	arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_SCTLR, reg);
}

/* A52_PHASE253_KGSL_SMMU_DOMAIN_CONTRACT_V1
 * Qualcomm KGSL creates one real/global domain and then dynamic per-process
 * page-table domains which share that context bank.  The dynamic domains own
 * page tables and ASIDs only; they must never rewrite the stream-to-CB map.
 */
static bool a52_kgsl_dynamic_domain(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);

	return !!(smmu_domain->attributes & BIT(DOMAIN_ATTR_DYNAMIC));
}

static bool a52_kgsl_procid_domain(struct arm_smmu_domain *smmu_domain)
{
	return !!(smmu_domain->attributes & BIT(DOMAIN_ATTR_PROCID));
}

static int a52_kgsl_init_asid(struct iommu_domain *domain,
			      struct arm_smmu_device *smmu)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	int ret;

	if (smmu_domain->stage == ARM_SMMU_DOMAIN_S2) {
		cfg->vmid = cfg->cbndx + 1;
		return 0;
	}

	if (!a52_kgsl_dynamic_domain(domain)) {
		/* Preserve ACK behavior for unrelated domains. KGSL's downstream
		 * contract reserves ASID zero and uses cbndx+1 for real KGSL CBs.
		 */
		cfg->asid = a52_kgsl_procid_domain(smmu_domain) ?
			cfg->cbndx + 1 : cfg->cbndx;
		return 0;
	}

	mutex_lock(&smmu->kgsl_asid_lock);
	ret = idr_alloc_cyclic(&smmu->kgsl_asid_idr, domain,
			       smmu->num_context_banks + 2,
			       A52_KGSL_MAX_ASID + 1, GFP_KERNEL);
	mutex_unlock(&smmu->kgsl_asid_lock);
	if (ret < 0)
		return ret;

	cfg->asid = ret;
	return 0;
}

static void a52_kgsl_free_asid(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;

	if (!smmu || !a52_kgsl_dynamic_domain(domain) ||
	    cfg->asid == A52_KGSL_INVALID_ASID)
		return;

	mutex_lock(&smmu->kgsl_asid_lock);
	idr_remove(&smmu->kgsl_asid_idr, cfg->asid);
	mutex_unlock(&smmu->kgsl_asid_lock);
	cfg->asid = A52_KGSL_INVALID_ASID;
}


/* A52_PHASE427_CB2_HANDOFF_ROAD_PROBE_V1
 *
 * Functional A/B treatment, deliberately scoped to the one non-secure
 * display master whose firmware handoff was measured by Phase426:
 *   qcom,smmu_sde_unsec, SID 0x800, translating pinned SME, firmware CB2.
 *
 * TouchGrass reuses that inherited context bank.  The GKI Qualcomm importer
 * reserves/pins it, then normal allocation picks CB4 and arm_smmu_domain_add_master()
 * rewrites the stream.  Phase427 returns the inherited CB only for this exact
 * master.  SID 0x801 and every other firmware stream are untouched.
 *
 * The existing earlymap path remains authoritative: CB2 is programmed with
 * the new TTBR/TCR/MAIR while SCTLR.M is clear, then the existing
 * DOMAIN_ATTR_EARLY_MAP transition enables translation after mappings exist.
 * A context TLBI is issued while translation is disabled after CB2 is fully
 * programmed.  No other SMMU policy is changed.
 */
/* A52_PHASE446_CB2_SWITCH */
extern void a52_p446_mark(u32 event,u32 aux0,u32 aux1);
extern void a52_p446_set_cb2_state(u32 enabled,u32 actual_cb);
extern void a52_p446_note_smmu(u32 event,u32 smr,u32 s2cr,u32 cb,u32 sctlr,u64 ttbr0,u32 tcr,u32 fsr,u32 aux);
static bool a52_p446_cb2_reuse = true;
static int __init a52_p446_cb2_setup(char *s)
{
    bool v=true;
    if (s && kstrtobool(s,&v)) return 0;
    a52_p446_cb2_reuse=v;
    a52_p446_set_cb2_state(v ? 1U : 0U, ~0U);
    return 1;
}
__setup("a52.cb2_reuse=",a52_p446_cb2_setup);

static struct arm_smmu_device *a52_p446b_sample_smmu;
static int a52_p446b_sample_sme = -1;

/* A52_PHASE446B_LIVE_CB_API */
int a52_p446_read_cb2(u32 *smr,u32 *s2cr,u32 *cb,u32 *sctlr,
                      u64 *ttbr0,u32 *tcr,u32 *fsr)
{
    struct arm_smmu_device *smmu=READ_ONCE(a52_p446b_sample_smmu);
    int sme=READ_ONCE(a52_p446b_sample_sme);
    u32 sr=~0U,s2=~0U,c=~0U;
    int ret;

    if(!smr || !s2cr || !cb || !sctlr || !ttbr0 || !tcr || !fsr)
        return -EINVAL;
    if(!smmu || sme<0 || sme>=smmu->num_mapping_groups)
        return -ENODEV;

    ret=arm_smmu_rpm_get(smmu);
    if(ret<0)
        return ret;

    if(smmu->smrs)
        sr=arm_smmu_gr0_read(smmu,ARM_SMMU_GR0_SMR(sme));
    s2=arm_smmu_gr0_read(smmu,ARM_SMMU_GR0_S2CR(sme));
    c=FIELD_GET(ARM_SMMU_S2CR_CBNDX,s2);
    if(c>=smmu->num_context_banks){
        arm_smmu_rpm_put(smmu);
        return -ERANGE;
    }

    *smr=sr; *s2cr=s2; *cb=c;
    *sctlr=arm_smmu_cb_read(smmu,c,ARM_SMMU_CB_SCTLR);
    *ttbr0=arm_smmu_cb_readq(smmu,c,ARM_SMMU_CB_TTBR0);
    *tcr=arm_smmu_cb_read(smmu,c,ARM_SMMU_CB_TCR);
    *fsr=arm_smmu_cb_read(smmu,c,ARM_SMMU_CB_FSR);
    arm_smmu_rpm_put(smmu);
    return 0;
}
EXPORT_SYMBOL_GPL(a52_p446_read_cb2);

static void a52_p446_emit_smmu(struct arm_smmu_device *smmu,u32 event,int sme,u32 cb,u32 aux)
{
    u32 smr=~0U,s2cr=~0U,sctlr=~0U,tcr=~0U,fsr=~0U; u64 ttbr=~0ULL; int ret;
    if(!smmu || sme<0 || sme>=smmu->num_mapping_groups || cb>=smmu->num_context_banks) return;
    ret=arm_smmu_rpm_get(smmu); if(ret<0)return;
    if(smmu->smrs) smr=arm_smmu_gr0_read(smmu,ARM_SMMU_GR0_SMR(sme));
    s2cr=arm_smmu_gr0_read(smmu,ARM_SMMU_GR0_S2CR(sme));
    if(smmu->smrs && FIELD_GET(ARM_SMMU_SMR_ID,smr)==0x800){
        WRITE_ONCE(a52_p446b_sample_smmu,smmu);
        WRITE_ONCE(a52_p446b_sample_sme,sme);
    }
    sctlr=arm_smmu_cb_read(smmu,cb,ARM_SMMU_CB_SCTLR); tcr=arm_smmu_cb_read(smmu,cb,ARM_SMMU_CB_TCR); fsr=arm_smmu_cb_read(smmu,cb,ARM_SMMU_CB_FSR); ttbr=arm_smmu_cb_readq(smmu,cb,ARM_SMMU_CB_TTBR0);
    arm_smmu_rpm_put(smmu); a52_p446_set_cb2_state(a52_p446_cb2_reuse ? 1U:0U,cb); a52_p446_note_smmu(event,smr,s2cr,cb,sctlr,ttbr,tcr,fsr,aux);
}


struct a52_p427_handoff_state {
	struct arm_smmu_device *smmu;
	struct iommu_domain *domain;
	u32 seen;
	u32 sid;
	s32 sme;
	u32 old_cb;
	u32 chosen_cb;
	u32 pinned;
	u32 old_type;
	u32 old_count;
	u32 context_reserved;
	u32 used;
	u32 irptndx;
	s32 irq;
	s32 request_ret;
};

static struct a52_p427_handoff_state a52_p427_handoff;

static bool a52_p427_sid800_dev(struct device *dev,
				struct iommu_fwspec **fw_out,
				struct arm_smmu_master_cfg **cfg_out)
{
	struct iommu_fwspec *fwspec;
	struct arm_smmu_master_cfg *cfg;
	u16 sid;

	if (!dev || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node, "qcom,smmu_sde_unsec"))
		return false;

	fwspec = dev_iommu_fwspec_get(dev);
	cfg = dev_iommu_priv_get(dev);
	if (!fwspec || !cfg || fwspec->num_ids != 1)
		return false;

	sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[0]);
	if (sid != 0x800)
		return false;

	if (fw_out)
		*fw_out = fwspec;
	if (cfg_out)
		*cfg_out = cfg;
	return true;
}

static int a52_p427_try_handoff_cb(struct arm_smmu_device *smmu,
				   struct device *dev, unsigned int start)
{
	struct iommu_fwspec *fwspec = NULL;
	struct arm_smmu_master_cfg *cfg = NULL;
	struct arm_smmu_s2cr *s2cr;
	int idx;
	u8 cb;

	if (!a52_p446_cb2_reuse) return -ENOENT;
	if (!a52_p427_sid800_dev(dev, &fwspec, &cfg))
		return -ENOENT;

	idx = cfg->smendx[0];
	memset(&a52_p427_handoff, 0, sizeof(a52_p427_handoff));
	a52_p427_handoff.smmu = smmu;
	a52_p427_handoff.seen = 1;
	a52_p427_handoff.sid = 0x800;
	a52_p427_handoff.sme = idx;

	if (idx == INVALID_SMENDX || idx < 0 ||
	    idx >= smmu->num_mapping_groups)
		return -ENOENT;

	s2cr = &smmu->s2crs[idx];
	cb = s2cr->cbndx;
	a52_p427_handoff.old_cb = cb;
	a52_p427_handoff.chosen_cb = cb;
	a52_p427_handoff.pinned = s2cr->pinned ? 1U : 0U;
	a52_p427_handoff.old_type = (u32)s2cr->type;
	a52_p427_handoff.old_count = (u32)s2cr->count;
	a52_p427_handoff.context_reserved =
		test_bit(cb, smmu->context_map) ? 1U : 0U;

	/* Fail closed to the original allocator unless every measured Phase426
	 * handoff contract is present.  This prevents Phase427 from commandeering
	 * an arbitrary CB on a different boot/DT/SMMU configuration.
	 */
	/* TouchGrass handoff deliberately overrides the normal stage allocator
	 * start index. The firmware-selected CB is the contract being tested.
	 */
	(void)start;
	if (!s2cr->pinned || s2cr->type != S2CR_TYPE_TRANS ||
	    cb != 2U || cb >= smmu->num_context_banks ||
	    !test_bit(cb, smmu->context_map))
		return -ENOENT;

	a52_p427_handoff.used = 1;
	return cb;
}

static bool a52_p427_reused_cb2(struct device *dev, u32 cb)
{
	return cb == 2U && a52_p427_handoff.used &&
	       a52_p427_sid800_dev(dev, NULL, NULL);
}

void a52_p427_smmu_dump(void)
{
	struct a52_p427_handoff_state *h = &a52_p427_handoff;
	struct arm_smmu_device *smmu = READ_ONCE(h->smmu);
	u32 smr = ~0U, s2cr = ~0U, sctlr = ~0U, fsr = ~0U;
	u32 tcr = ~0U, mair0 = ~0U;
	u64 ttbr0 = ~0ULL;
	int ret = -ENODEV;

	if (smmu && h->sme >= 0 && h->sme < smmu->num_mapping_groups &&
	    h->chosen_cb < smmu->num_context_banks) {
		ret = arm_smmu_rpm_get(smmu);
		if (ret >= 0) {
			smr = smmu->smrs ? arm_smmu_gr0_read(smmu,
				ARM_SMMU_GR0_SMR(h->sme)) : 0;
			s2cr = arm_smmu_gr0_read(smmu,
				ARM_SMMU_GR0_S2CR(h->sme));
			sctlr = arm_smmu_cb_read(smmu, h->chosen_cb,
				ARM_SMMU_CB_SCTLR);
			fsr = arm_smmu_cb_read(smmu, h->chosen_cb,
				ARM_SMMU_CB_FSR);
			tcr = arm_smmu_cb_read(smmu, h->chosen_cb,
				ARM_SMMU_CB_TCR);
			ttbr0 = arm_smmu_cb_readq(smmu, h->chosen_cb,
				ARM_SMMU_CB_TTBR0);
			mair0 = arm_smmu_cb_read(smmu, h->chosen_cb,
				ARM_SMMU_CB_S1_MAIR0);
			arm_smmu_rpm_put(smmu);
		}
	}

	a52_ackfr_record("P427 H v=%u sid=%x e=%d old=%u cb=%u p=%u c=%u r=%u",
		h->seen, h->sid, h->sme, h->old_cb, h->chosen_cb,
		h->pinned, h->old_count, h->context_reserved);
	a52_ackfr_record("P427 C u=%u ir=%u irq=%d rr=%d rt=%d",
		h->used, h->irptndx, h->irq, h->request_ret, ret);
	a52_ackfr_record("P427 S smr=%x s2=%x cb=%u ty=%u",
		smr, s2cr, FIELD_GET(ARM_SMMU_S2CR_CBNDX, s2cr),
		FIELD_GET(ARM_SMMU_S2CR_TYPE, s2cr));
	a52_ackfr_record("P427 X sc=%x fs=%x tc=%x ma=%x",
		sctlr, fsr, tcr, mair0);
	a52_ackfr_record("P427 R tt=%llx",
		(unsigned long long)ttbr0);
}
EXPORT_SYMBOL_GPL(a52_p427_smmu_dump);


/* A52_PHASE430_SF_DMA_FORENSICS_V1
 * Passive SID800/CB2 state first, then one explicit ATOS translation.  The
 * software pgtable walk and hardware PAR result are kept separate so a hard
 * translation timeout cannot silently fall back and masquerade as success.
 */
struct a52_p430_smmu_diag {
	u32 valid, rpm_ret, sid, sme, cb, irq;
	u32 smr, s2cr, cbar, cba2r, cbfrsynra;
	u32 sctlr, actlr, tcr, tcr2, mair0, mair1, contextidr;
	u32 fsr, fsynr0, gfsr, gfsynr0, gfsynr1, gfsynr2;
	u32 tlbstatus, gtlbstatus, atos_status, atos_timeout;
	u64 ttbr0, ttbr1, far, sw_phys, hw_phys, par;
} __packed;

int a52_p430_smmu_forensic(u64 iova, void *buffer, size_t len)
{
	struct a52_p430_smmu_diag *o = buffer;
	struct a52_p427_handoff_state *h = &a52_p427_handoff;
	struct arm_smmu_device *smmu = READ_ONCE(h->smmu);
	struct iommu_domain *domain = READ_ONCE(h->domain);
	struct arm_smmu_domain *sd;
	struct arm_smmu_cfg *cfg;
	struct io_pgtable_ops *ops;
	void __iomem *reg;
	unsigned long flags;
	u32 tmp = ~0U;
	u64 va, par;
	int ret;

	if (!o || len < sizeof(*o))
		return -EINVAL;
	memset(o, 0, sizeof(*o));
	o->sid = h->sid;
	o->sme = (u32)h->sme;
	o->cb = h->chosen_cb;
	o->irq = (u32)h->irq;
	if (!smmu || !domain || h->sme < 0 ||
	    h->sme >= smmu->num_mapping_groups ||
	    h->chosen_cb >= smmu->num_context_banks)
		return -ENODEV;
	sd = to_smmu_domain(domain);
	cfg = &sd->cfg;
	ops = sd->pgtbl_ops;

	ret = arm_smmu_rpm_get(smmu);
	o->rpm_ret = (u32)ret;
	if (ret < 0)
		return ret;

	/* Passive snapshot before ATOS or any fault clear. */
	o->smr = smmu->smrs ? arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(h->sme)) : 0U;
	o->s2cr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_S2CR(h->sme));
	o->cbar = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBAR(cfg->cbndx));
	o->cba2r = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBA2R(cfg->cbndx));
	o->cbfrsynra = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBFRSYNRA(cfg->cbndx));
	o->sctlr = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_SCTLR);
	o->actlr = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_ACTLR);
	o->tcr = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_TCR);
	o->tcr2 = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_TCR2);
	o->ttbr0 = arm_smmu_cb_readq(smmu, cfg->cbndx, ARM_SMMU_CB_TTBR0);
	o->ttbr1 = arm_smmu_cb_readq(smmu, cfg->cbndx, ARM_SMMU_CB_TTBR1);
	o->mair0 = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_S1_MAIR0);
	o->mair1 = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_S1_MAIR1);
	o->contextidr = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_CONTEXTIDR);
	o->fsr = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_FSR);
	o->fsynr0 = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_FSYNR0);
	o->far = arm_smmu_cb_readq(smmu, cfg->cbndx, ARM_SMMU_CB_FAR);
	o->gfsr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSR);
	o->gfsynr0 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR0);
	o->gfsynr1 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR1);
	o->gfsynr2 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR2);
	o->tlbstatus = arm_smmu_cb_read(smmu, cfg->cbndx, ARM_SMMU_CB_TLBSTATUS);
	o->gtlbstatus = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sTLBGSTATUS);
	if (ops)
		o->sw_phys = (u64)ops->iova_to_phys(ops, (unsigned long)iova);

	/* Explicit hardware ATOS after the passive image is complete. */
	spin_lock_irqsave(&sd->cb_lock, flags);
	va = iova & ~0xfffULL;
	if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)
		arm_smmu_cb_writeq(smmu, cfg->cbndx, ARM_SMMU_CB_ATS1PR, va);
	else
		arm_smmu_cb_write(smmu, cfg->cbndx, ARM_SMMU_CB_ATS1PR, (u32)va);
	reg = arm_smmu_page(smmu, ARM_SMMU_CB(smmu, cfg->cbndx)) + ARM_SMMU_CB_ATSR;
	if (readl_poll_timeout_atomic(reg, tmp, !(tmp & ARM_SMMU_ATSR_ACTIVE), 5, 50)) {
		o->atos_timeout = 1U;
		o->atos_status = tmp;
		spin_unlock_irqrestore(&sd->cb_lock, flags);
	} else {
		o->atos_status = tmp;
		par = arm_smmu_cb_readq(smmu, cfg->cbndx, ARM_SMMU_CB_PAR);
		o->par = par;
		if (!(par & ARM_SMMU_CB_PAR_F))
			o->hw_phys = (par & GENMASK_ULL(39, 12)) | (iova & 0xfffULL);
		spin_unlock_irqrestore(&sd->cb_lock, flags);
	}
	o->valid = 1U;
	arm_smmu_rpm_put(smmu);
	return 0;
}
EXPORT_SYMBOL_GPL(a52_p430_smmu_forensic);


static int arm_smmu_alloc_context_bank(struct arm_smmu_domain *smmu_domain,
				       struct arm_smmu_device *smmu,
				       struct device *dev, unsigned int start)
{
	int a52_p427_cb;

	a52_p427_cb = a52_p427_try_handoff_cb(smmu, dev, start);
	if (a52_p427_cb >= 0)
		return a52_p427_cb;
	if (a52_p427_cb != -ENOENT)
		return a52_p427_cb;

	if (smmu->impl && smmu->impl->alloc_context_bank)
		return smmu->impl->alloc_context_bank(smmu_domain, smmu, dev, start);

	return __arm_smmu_alloc_bitmap(smmu->context_map, start, smmu->num_context_banks);
}

static int arm_smmu_init_domain_context(struct iommu_domain *domain,
					struct arm_smmu_device *smmu,
					struct device *dev)
{
	int irq, start, ret = 0;
	unsigned long ias, oas;
	struct io_pgtable_ops *pgtbl_ops;
	struct io_pgtable_cfg pgtbl_cfg;
	enum io_pgtable_fmt fmt;
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	irqreturn_t (*context_fault)(int irq, void *dev);
	bool dynamic = a52_kgsl_dynamic_domain(domain);

	mutex_lock(&smmu_domain->init_mutex);
	if (smmu_domain->smmu)
		goto out_unlock;

	if (domain->type == IOMMU_DOMAIN_IDENTITY) {
		smmu_domain->stage = ARM_SMMU_DOMAIN_BYPASS;
		smmu_domain->smmu = smmu;
		goto out_unlock;
	}

	if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S1))
		smmu_domain->stage = ARM_SMMU_DOMAIN_S2;
	if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S2))
		smmu_domain->stage = ARM_SMMU_DOMAIN_S1;

	if (smmu->features & ARM_SMMU_FEAT_FMT_AARCH32_L)
		cfg->fmt = ARM_SMMU_CTX_FMT_AARCH32_L;
	if (IS_ENABLED(CONFIG_IOMMU_IO_PGTABLE_ARMV7S) &&
	    !IS_ENABLED(CONFIG_64BIT) && !IS_ENABLED(CONFIG_ARM_LPAE) &&
	    (smmu->features & ARM_SMMU_FEAT_FMT_AARCH32_S) &&
	    (smmu_domain->stage == ARM_SMMU_DOMAIN_S1))
		cfg->fmt = ARM_SMMU_CTX_FMT_AARCH32_S;
	if ((IS_ENABLED(CONFIG_64BIT) || cfg->fmt == ARM_SMMU_CTX_FMT_NONE) &&
	    (smmu->features & (ARM_SMMU_FEAT_FMT_AARCH64_64K |
			       ARM_SMMU_FEAT_FMT_AARCH64_16K |
			       ARM_SMMU_FEAT_FMT_AARCH64_4K)))
		cfg->fmt = ARM_SMMU_CTX_FMT_AARCH64;

	if (cfg->fmt == ARM_SMMU_CTX_FMT_NONE) {
		ret = -EINVAL;
		goto out_unlock;
	}

	switch (smmu_domain->stage) {
	case ARM_SMMU_DOMAIN_S1:
		cfg->cbar = CBAR_TYPE_S1_TRANS_S2_BYPASS;
		start = smmu->num_s2_context_banks;
		ias = smmu->va_size;
		oas = smmu->ipa_size;
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64) {
			fmt = ARM_64_LPAE_S1;
			if (smmu->use_3lvl_tables)
				ias = min(ias, 39UL);
		} else if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_L) {
			fmt = ARM_32_LPAE_S1;
			ias = min(ias, 32UL);
			oas = min(oas, 40UL);
		} else {
			fmt = ARM_V7S;
			ias = min(ias, 32UL);
			oas = min(oas, 32UL);
		}
		smmu_domain->flush_ops = &arm_smmu_s1_tlb_ops;
		break;
	case ARM_SMMU_DOMAIN_NESTED:
	case ARM_SMMU_DOMAIN_S2:
		cfg->cbar = CBAR_TYPE_S2_TRANS;
		start = 0;
		ias = smmu->ipa_size;
		oas = smmu->pa_size;
		if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64) {
			fmt = ARM_64_LPAE_S2;
		} else {
			fmt = ARM_32_LPAE_S2;
			ias = min(ias, 40UL);
			oas = min(oas, 40UL);
		}
		if (smmu->version == ARM_SMMU_V2)
			smmu_domain->flush_ops = &arm_smmu_s2_tlb_ops_v2;
		else
			smmu_domain->flush_ops = &arm_smmu_s2_tlb_ops_v1;
		break;
	default:
		ret = -EINVAL;
		goto out_unlock;
	}

	if (of_device_is_compatible(smmu->dev->of_node, "qcom,smmu-v2"))
		a52_ackfr_record("K254 C pre t=%u d=%d used=%u/%u s2=%u dev=%s",
			domain->type, dynamic,
			bitmap_weight(smmu->context_map, smmu->num_context_banks),
			smmu->num_context_banks, smmu->num_s2_context_banks,
			dev_name(dev));

	if (dynamic) {
		if (cfg->cbndx == A52_KGSL_INVALID_CBNDX ||
		    cfg->cbndx >= smmu->num_context_banks)
			ret = -EINVAL;
		else
			ret = cfg->cbndx;
	} else {
		ret = arm_smmu_alloc_context_bank(smmu_domain, smmu, dev, start);
	}

	if (of_device_is_compatible(smmu->dev->of_node, "qcom,smmu-v2"))
		a52_ackfr_record("K254 C alloc t=%u d=%d rc=%d used=%u/%u",
			domain->type, dynamic, ret,
			bitmap_weight(smmu->context_map, smmu->num_context_banks),
			smmu->num_context_banks);
	if (ret < 0)
		goto out_unlock;

	smmu_domain->smmu = smmu;
	cfg->cbndx = ret;

	ret = a52_kgsl_init_asid(domain, smmu);
	if (of_device_is_compatible(smmu->dev->of_node, "qcom,smmu-v2"))
		a52_ackfr_record("K254 A asid t=%u d=%d cb=%u asid=%u rc=%d",
			domain->type, dynamic, cfg->cbndx, cfg->asid, ret);
	if (ret)
		goto out_clear_smmu;

	if (dynamic) {
		cfg->irptndx = ARM_SMMU_INVALID_IRPTNDX;
	} else if (smmu->version < ARM_SMMU_V2) {
		cfg->irptndx = atomic_inc_return(&smmu->irptndx);
		cfg->irptndx %= smmu->num_context_irqs;
	} else {
		cfg->irptndx = cfg->cbndx;
	}

	pgtbl_cfg = (struct io_pgtable_cfg) {
		.pgsize_bitmap	= smmu->pgsize_bitmap,
		.ias		= ias,
		.oas		= oas,
		.coherent_walk	= smmu->features & ARM_SMMU_FEAT_COHERENT_WALK,
		.tlb		= smmu_domain->flush_ops,
		.iommu_dev	= smmu->dev,
	};
	if (arm_smmu_has_secure_vmid(smmu_domain)) {
		pgtbl_cfg.alloc_pages_exact = arm_smmu_alloc_pages_exact;
		pgtbl_cfg.free_pages_exact = arm_smmu_free_pages_exact;
	}

	if (smmu->impl && smmu->impl->init_context) {
		ret = smmu->impl->init_context(smmu_domain, &pgtbl_cfg, dev);
		if (ret)
			goto out_free_asid;
	}

	if (smmu_domain->non_strict)
		pgtbl_cfg.quirks |= IO_PGTABLE_QUIRK_NON_STRICT;

	pgtbl_ops = alloc_io_pgtable_ops(fmt, &pgtbl_cfg, smmu_domain);
	if (!pgtbl_ops) {
		ret = -ENOMEM;
		arm_smmu_secure_domain_lock(smmu_domain);
		arm_smmu_secure_pool_destroy(smmu_domain);
		arm_smmu_unassign_table(smmu_domain);
		arm_smmu_secure_domain_unlock(smmu_domain);
		goto out_free_asid;
	}

	arm_smmu_secure_domain_lock(smmu_domain);
	ret = arm_smmu_assign_table(smmu_domain);
	if (ret) {
		free_io_pgtable_ops(pgtbl_ops);
		arm_smmu_secure_pool_destroy(smmu_domain);
		arm_smmu_unassign_table(smmu_domain);
	}
	arm_smmu_secure_domain_unlock(smmu_domain);
	if (ret)
		goto out_free_asid;

	/* alloc_io_pgtable_ops() fills TTBR/TCR/MAIR in this structure. */
	smmu_domain->pgtbl_cfg = pgtbl_cfg;

	domain->pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;
	if (pgtbl_cfg.quirks & IO_PGTABLE_QUIRK_ARM_TTBR1) {
		domain->geometry.aperture_start = ~0UL << ias;
		domain->geometry.aperture_end = ~0UL;
	} else {
		domain->geometry.aperture_end = (1UL << ias) - 1;
	}
	domain->geometry.force_aperture = true;

	if (!dynamic) {
		arm_smmu_init_context_bank(smmu_domain, &pgtbl_cfg);
		arm_smmu_write_context_bank(smmu, cfg->cbndx);
		/* CB2 is still under earlymap with SCTLR.M clear here.  Drop any
		 * stale firmware translations only after the complete new context
		 * has been written, before HLOS enables stage-1 translation.
		 */
		if (a52_p427_reused_cb2(dev, cfg->cbndx) &&
		    smmu_domain->stage == ARM_SMMU_DOMAIN_S1)
			arm_smmu_tlb_inv_context_s1(smmu_domain);

		irq = smmu->irqs[smmu->num_global_irqs + cfg->irptndx];
		if (smmu->impl && smmu->impl->context_fault)
			context_fault = smmu->impl->context_fault;
		else
			context_fault = arm_smmu_context_fault;

		ret = devm_request_irq(smmu->dev, irq, context_fault,
			       IRQF_SHARED, "arm-smmu-context-fault", domain);

		/* Cache first, publish later from Phase421 S00 after survival RAM is live. */
		if (a52_p422_dev_has_sid800(dev)) {
			a52_p422_irq_cache.domain = domain;
			a52_p422_irq_cache.smmu = smmu;
			a52_p422_irq_cache.sid = 0x800;
			a52_p422_irq_cache.cbndx = cfg->cbndx;
			a52_p422_irq_cache.irptndx = cfg->irptndx;
			a52_p422_irq_cache.irq = irq;
			a52_p422_irq_cache.request_ret = ret;
			WRITE_ONCE(a52_p422_irq_cache.valid, true);
		
			if (a52_p427_reused_cb2(dev, cfg->cbndx)) {
				a52_p427_handoff.domain = domain;
				a52_p427_handoff.irptndx = cfg->irptndx;
				a52_p427_handoff.irq = irq;
				a52_p427_handoff.request_ret = ret;
			}
		}
		if (ret < 0) {
			dev_err(smmu->dev, "failed to request context IRQ %d (%u)\n",
				cfg->irptndx, irq);
			cfg->irptndx = ARM_SMMU_INVALID_IRPTNDX;
		}
	}

	mutex_unlock(&smmu_domain->init_mutex);
	smmu_domain->pgtbl_ops = pgtbl_ops;

	if (of_device_is_compatible(smmu->dev->of_node, "qcom,smmu-v2"))
		a52_ackfr_record("K254 D ok t=%u d=%d cb=%u asid=%u used=%u",
			domain->type, dynamic, cfg->cbndx, cfg->asid,
			bitmap_weight(smmu->context_map, smmu->num_context_banks));
	if (a52_apps_smmu(smmu->dev) || a52_kgsl_procid_domain(smmu_domain))
		a52_ackfr_record("K253 D init dyn=%d cb=%u asid=%u proc=%u",
			dynamic, cfg->cbndx, cfg->asid, cfg->procid);
	return 0;

out_free_asid:
	if (dynamic)
		a52_kgsl_free_asid(domain);
out_clear_smmu:
	if (!dynamic && !a52_p427_reused_cb2(dev, cfg->cbndx))
		__arm_smmu_free_bitmap(smmu->context_map, cfg->cbndx);
	smmu_domain->smmu = NULL;
out_unlock:
	mutex_unlock(&smmu_domain->init_mutex);
	return ret;
}


static void arm_smmu_destroy_domain_context(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	bool dynamic = a52_kgsl_dynamic_domain(domain);
	int ret, irq;

	if (!smmu || domain->type == IOMMU_DOMAIN_IDENTITY)
		return;

	ret = arm_smmu_rpm_get(smmu);
	if (ret < 0)
		return;

	if (dynamic) {
		a52_kgsl_free_asid(domain);
		arm_smmu_secure_domain_lock(smmu_domain);
		free_io_pgtable_ops(smmu_domain->pgtbl_ops);
		arm_smmu_secure_pool_destroy(smmu_domain);
		arm_smmu_unassign_table(smmu_domain);
		arm_smmu_secure_domain_unlock(smmu_domain);
		smmu_domain->pgtbl_ops = NULL;
		smmu_domain->smmu = NULL;
		arm_smmu_rpm_put(smmu);
		return;
	}

	smmu->cbs[cfg->cbndx].cfg = NULL;
	arm_smmu_write_context_bank(smmu, cfg->cbndx);

	if (cfg->irptndx != ARM_SMMU_INVALID_IRPTNDX) {
		irq = smmu->irqs[smmu->num_global_irqs + cfg->irptndx];
		devm_free_irq(smmu->dev, irq, domain);
	}

	arm_smmu_secure_domain_lock(smmu_domain);
	free_io_pgtable_ops(smmu_domain->pgtbl_ops);
	arm_smmu_secure_pool_destroy(smmu_domain);
	arm_smmu_unassign_table(smmu_domain);
	arm_smmu_secure_domain_unlock(smmu_domain);
	__arm_smmu_free_bitmap(smmu->context_map, cfg->cbndx);

	arm_smmu_rpm_put(smmu);
}


static struct iommu_domain *arm_smmu_domain_alloc(unsigned type)
{
	struct arm_smmu_domain *smmu_domain;

	if (type != IOMMU_DOMAIN_UNMANAGED &&
	    type != IOMMU_DOMAIN_DMA &&
	    type != IOMMU_DOMAIN_IDENTITY)
		return NULL;
	/*
	 * Allocate the domain and initialise some of its data structures.
	 * We can't really do anything meaningful until we've added a
	 * master.
	 */
	smmu_domain = kzalloc(sizeof(*smmu_domain), GFP_KERNEL);
	if (!smmu_domain)
		return NULL;

	if (type == IOMMU_DOMAIN_DMA && (using_legacy_binding ||
	    iommu_get_dma_cookie(&smmu_domain->domain))) {
		kfree(smmu_domain);
		return NULL;
	}

	smmu_domain->secure_vmid = VMID_INVAL;
	smmu_domain->cfg.cbndx = A52_KGSL_INVALID_CBNDX;
	smmu_domain->cfg.irptndx = ARM_SMMU_INVALID_IRPTNDX;
	smmu_domain->cfg.asid = A52_KGSL_INVALID_ASID;
	INIT_LIST_HEAD(&smmu_domain->pte_info_list);
	INIT_LIST_HEAD(&smmu_domain->unassign_list);
	INIT_LIST_HEAD(&smmu_domain->secure_pool_list);
	mutex_init(&smmu_domain->assign_lock);
	mutex_init(&smmu_domain->init_mutex);
	spin_lock_init(&smmu_domain->cb_lock);

	return &smmu_domain->domain;
}

static void arm_smmu_domain_free(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);

	/*
	 * Free the domain resources. We assume that all devices have
	 * already been detached.
	 */
	iommu_put_dma_cookie(domain);
	arm_smmu_destroy_domain_context(domain);
	kfree(smmu_domain);
}

static void arm_smmu_write_smr(struct arm_smmu_device *smmu, int idx)
{
	struct arm_smmu_smr *smr = smmu->smrs + idx;
	u32 reg = FIELD_PREP(ARM_SMMU_SMR_ID, smr->id) |
		  FIELD_PREP(ARM_SMMU_SMR_MASK, smr->mask);

	if (!(smmu->features & ARM_SMMU_FEAT_EXIDS) && smr->valid)
		reg |= ARM_SMMU_SMR_VALID;
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(idx), reg);
}

static void arm_smmu_write_s2cr(struct arm_smmu_device *smmu, int idx)
{
	struct arm_smmu_s2cr *s2cr = smmu->s2crs + idx;
	u32 reg;

	if (smmu->impl && smmu->impl->write_s2cr) {
		smmu->impl->write_s2cr(smmu, idx);
		return;
	}

	reg = FIELD_PREP(ARM_SMMU_S2CR_TYPE, s2cr->type) |
	      FIELD_PREP(ARM_SMMU_S2CR_CBNDX, s2cr->cbndx) |
	      FIELD_PREP(ARM_SMMU_S2CR_PRIVCFG, s2cr->privcfg);

	if (smmu->features & ARM_SMMU_FEAT_EXIDS && smmu->smrs &&
	    smmu->smrs[idx].valid)
		reg |= ARM_SMMU_S2CR_EXIDVALID;
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_S2CR(idx), reg);
}

static void arm_smmu_write_sme(struct arm_smmu_device *smmu, int idx)
{
	arm_smmu_write_s2cr(smmu, idx);
	if (smmu->smrs)
		arm_smmu_write_smr(smmu, idx);
}

/*
 * The width of SMR's mask field depends on sCR0_EXIDENABLE, so this function
 * should be called after sCR0 is written.
 */
static void arm_smmu_test_smr_masks(struct arm_smmu_device *smmu)
{
	u32 smr;
	int i;

	if (!smmu->smrs)
		return;
	/*
	 * If we've had to accommodate firmware memory regions, we may
	 * have live SMRs by now; tread carefully...
	 *
	 * Somewhat perversely, not having a free SMR for this test implies we
	 * can get away without it anyway, as we'll only be able to 'allocate'
	 * these SMRs for the ID/mask values we're already trusting to be OK.
	 */
	for (i = 0; i < smmu->num_mapping_groups; i++)
		if (!smmu->smrs[i].valid)
			goto smr_ok;
	return;
smr_ok:
	if (a52_p426_is_target(smmu)) {
		a52_p426_mask_test_idx = i;
		a52_p426_mask_test_before =
			arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
	}
	/*
	 * SMR.ID bits may not be preserved if the corresponding MASK
	 * bits are set, so check each one separately. We can reject
	 * masters later if they try to claim IDs outside these masks.
	 */
	smr = FIELD_PREP(ARM_SMMU_SMR_ID, smmu->streamid_mask);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(i), smr);
	smr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
	smmu->streamid_mask = FIELD_GET(ARM_SMMU_SMR_ID, smr);

	smr = FIELD_PREP(ARM_SMMU_SMR_MASK, smmu->streamid_mask);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(i), smr);
	smr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
	smmu->smr_mask_mask = FIELD_GET(ARM_SMMU_SMR_MASK, smr);
	if (a52_p426_is_target(smmu))
		a52_p426_mask_test_after =
			arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
}

static int arm_smmu_find_sme(struct arm_smmu_device *smmu, u16 id, u16 mask)
{
	struct arm_smmu_smr *smrs = smmu->smrs;
	int i, free_idx = -ENOSPC;

	/* Stream indexing is blissfully easy */
	if (!smrs)
		return id;

	/* Validating SMRs is... less so */
	for (i = 0; i < smmu->num_mapping_groups; ++i) {
		if (!smrs[i].valid) {
			/*
			 * Note the first free entry we come across, which
			 * we'll claim in the end if nothing else matches.
			 */
			if (free_idx < 0)
				free_idx = i;
			continue;
		}
		/*
		 * If the new entry is _entirely_ matched by an existing entry,
		 * then reuse that, with the guarantee that there also cannot
		 * be any subsequent conflicting entries. In normal use we'd
		 * expect simply identical entries for this case, but there's
		 * no harm in accommodating the generalisation.
		 */
		if ((mask & smrs[i].mask) == mask &&
		    !((id ^ smrs[i].id) & ~smrs[i].mask))
			return i;
		/*
		 * If the new entry has any other overlap with an existing one,
		 * though, then there always exists at least one stream ID
		 * which would cause a conflict, and we can't allow that risk.
		 */
		if (!((id ^ smrs[i].id) & ~(smrs[i].mask | mask)))
			return -EINVAL;
	}

	return free_idx;
}

static bool arm_smmu_free_sme(struct arm_smmu_device *smmu, int idx)
{
	bool pinned = smmu->s2crs[idx].pinned;
	u8 cbndx = smmu->s2crs[idx].cbndx;

	if (--smmu->s2crs[idx].count)
		return false;

	smmu->s2crs[idx] = s2cr_init_val;
	if (pinned) {
		smmu->s2crs[idx].pinned = true;
		smmu->s2crs[idx].cbndx = cbndx;
	} else if (smmu->smrs) {
		smmu->smrs[idx].valid = false;
	}

	return true;
}

static int arm_smmu_master_alloc_smes(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct arm_smmu_device *smmu = cfg->smmu;
	struct arm_smmu_smr *smrs = smmu->smrs;
	int i, idx, ret;

	mutex_lock(&smmu->stream_map_mutex);
	/* Figure out a viable stream map entry allocation */
	for_each_cfg_sme(cfg, fwspec, i, idx) {
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);
		u16 mask = FIELD_GET(ARM_SMMU_SMR_MASK, fwspec->ids[i]);

		if (idx != INVALID_SMENDX) {
			ret = -EEXIST;
			goto out_err;
		}

		ret = arm_smmu_find_sme(smmu, sid, mask);
		if (ret < 0)
			goto out_err;

		idx = ret;
		if (smrs && smmu->s2crs[idx].count == 0) {
			smrs[idx].id = sid;
			smrs[idx].mask = mask;
			smrs[idx].valid = true;
		}
		smmu->s2crs[idx].count++;
		cfg->smendx[i] = (s16)idx;
	}

	/* It worked! Now, poke the actual hardware */
	for_each_cfg_sme(cfg, fwspec, i, idx)
		arm_smmu_write_sme(smmu, idx);

	mutex_unlock(&smmu->stream_map_mutex);
	return 0;

out_err:
	while (i--) {
		arm_smmu_free_sme(smmu, cfg->smendx[i]);
		cfg->smendx[i] = INVALID_SMENDX;
	}
	mutex_unlock(&smmu->stream_map_mutex);
	return ret;
}

static void arm_smmu_master_free_smes(struct arm_smmu_master_cfg *cfg,
				      struct iommu_fwspec *fwspec)
{
	struct arm_smmu_device *smmu = cfg->smmu;
	int i, idx;

	mutex_lock(&smmu->stream_map_mutex);
	for_each_cfg_sme(cfg, fwspec, i, idx) {
		if (arm_smmu_free_sme(smmu, idx))
			arm_smmu_write_sme(smmu, idx);
		cfg->smendx[i] = INVALID_SMENDX;
	}
	mutex_unlock(&smmu->stream_map_mutex);
}

static int arm_smmu_domain_add_master(struct arm_smmu_domain *smmu_domain,
				      struct arm_smmu_master_cfg *cfg,
				      struct iommu_fwspec *fwspec)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_s2cr *s2cr = smmu->s2crs;
	u8 cbndx = smmu_domain->cfg.cbndx;
	enum arm_smmu_s2cr_type type;
	int i, idx;

	if (smmu_domain->stage == ARM_SMMU_DOMAIN_BYPASS)
		type = S2CR_TYPE_BYPASS;
	else
		type = S2CR_TYPE_TRANS;

	for_each_cfg_sme(cfg, fwspec, i, idx) {
		if (type == s2cr[idx].type && cbndx == s2cr[idx].cbndx)
			continue;

		/* Don't bypasss pinned streams; leave them as they are */
		if (type == S2CR_TYPE_BYPASS && s2cr[idx].pinned)
			continue;

		if (fwspec->num_ids == 1 && FIELD_GET(ARM_SMMU_SMR_ID,fwspec->ids[0]) == 0x800)
			a52_p446_emit_smmu(smmu,8U,idx,s2cr[idx].cbndx,cbndx);
		s2cr[idx].type = type;
		s2cr[idx].privcfg = S2CR_PRIVCFG_DEFAULT;
		s2cr[idx].cbndx = cbndx;
		arm_smmu_write_s2cr(smmu, idx);
		if (fwspec->num_ids == 1 && FIELD_GET(ARM_SMMU_SMR_ID,fwspec->ids[0]) == 0x800)
			a52_p446_emit_smmu(smmu,9U,idx,cbndx,0U);
	}
	return 0;
}

static void a52_arm_smmu_apply_dt_domain_attrs(
		struct iommu_domain *domain,
		struct arm_smmu_domain *smmu_domain, struct device *dev)
{
	struct device_node *np;
	const char *iommu_dma;

	if (!dev->of_node)
		return;

	np = of_parse_phandle(dev->of_node, "qcom,iommu-group", 0);
	if (!np)
		np = of_node_get(dev->of_node);

	/* A52_PHASE254_KGSL_DISABLED_DEFAULT_DOMAIN_V1
	 * TouchGrass calls its default-domain policy only for IOMMU_DOMAIN_DMA.
	 * qcom,iommu-dma="disabled" means a software-only dynamic domain: keep
	 * a valid logical CB index but do not own/program a real CB or stream.
	 */
	if (domain->type == IOMMU_DOMAIN_DMA &&
	    !of_property_read_string(np, "qcom,iommu-dma", &iommu_dma) &&
	    !strcmp(iommu_dma, "disabled")) {
		smmu_domain->attributes |= BIT(DOMAIN_ATTR_DYNAMIC);
		smmu_domain->cfg.cbndx = 0;
		a52_ackfr_record("K254 D disabled dev=%s cb=0", dev_name(dev));
	}

	if (of_property_read_bool(np, "qcom,iommu-earlymap"))
		smmu_domain->attributes |= BIT(DOMAIN_ATTR_EARLY_MAP);

	if (of_property_match_string(np, "qcom,iommu-faults",
				     "non-fatal") >= 0)
		smmu_domain->attributes |= BIT(DOMAIN_ATTR_NON_FATAL_FAULTS);

	if (of_property_read_u32(np, "qcom,iommu-vmid",
				 &smmu_domain->secure_vmid))
		smmu_domain->secure_vmid = VMID_INVAL;

	of_node_put(np);
}


/* A52_PHASE278_LIVE_DISPLAY_SMMU_SNAPSHOT_V1
 * Diagnostic-only runtime correlation for the Phase277 display ACTLR experiment.
 * Remember the exact display SME/context selected during attach, then expose a
 * read-only snapshot callable from the already-proven DSI DMA failure boundary.
 * No SMMU stream, context, ACTLR, S2CR, SMR, page-table or TBU state is changed.
 */
struct a52_p278_display_ctx {
	struct arm_smmu_device *smmu;
	u16 sid;
	s16 sme;
	u8 cbndx;
	bool valid;
};

static struct a52_p278_display_ctx a52_p278_display_ctx[2];

static void a52_p278_remember_display_context(
		struct arm_smmu_domain *smmu_domain,
		struct iommu_fwspec *fwspec, struct device *dev)
{
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	int i;

	if (!cfg || !smmu || !fwspec)
		return;

	for (i = 0; i < fwspec->num_ids; i++) {
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);
		int slot, sme;

		if (sid != 0x800 && sid != 0x801)
			continue;
		slot = sid - 0x800;
		sme = cfg->smendx[i];
		if (sme == INVALID_SMENDX || sme < 0 ||
		    sme >= smmu->num_mapping_groups) {
			a52_ackfr_record("P276 278R s=%x e=%d x=1",
					  sid, sme);
			continue;
		}

		a52_p278_display_ctx[slot].smmu = smmu;
		a52_p278_display_ctx[slot].sid = sid;
		a52_p278_display_ctx[slot].sme = sme;
		a52_p278_display_ctx[slot].cbndx = smmu_domain->cfg.cbndx;
		a52_p278_display_ctx[slot].valid = true;
		a52_ackfr_record("P276 278R s=%x e=%d c=%u",
				  sid, sme, smmu_domain->cfg.cbndx);
	}
}

void a52_p278_display_smmu_snapshot(unsigned int point)
{
	bool any = false;
	int slot;

	for (slot = 0; slot < ARRAY_SIZE(a52_p278_display_ctx); slot++) {
		struct a52_p278_display_ctx *ctx = &a52_p278_display_ctx[slot];
		struct arm_smmu_device *smmu;
		u32 actlr, sctlr, fsr, s2cr, smr, cbar;
		u32 type, stream_cb;
		int ret;

		if (!ctx->valid)
			continue;
		any = true;
		smmu = ctx->smmu;
		if (!smmu || ctx->sme < 0 ||
		    ctx->sme >= smmu->num_mapping_groups ||
		    ctx->cbndx >= smmu->num_context_banks) {
			a52_ackfr_record("P276 278X q=%u s=%x x=1",
					  point, ctx->sid);
			continue;
		}

		ret = arm_smmu_rpm_get(smmu);
		if (ret < 0) {
			a52_ackfr_record("P276 278X q=%u s=%x r=%d",
					  point, ctx->sid, ret);
			continue;
		}

		actlr = arm_smmu_cb_read(smmu, ctx->cbndx, ARM_SMMU_CB_ACTLR);
		sctlr = arm_smmu_cb_read(smmu, ctx->cbndx, ARM_SMMU_CB_SCTLR);
		fsr = arm_smmu_cb_read(smmu, ctx->cbndx, ARM_SMMU_CB_FSR);
		s2cr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_S2CR(ctx->sme));
		smr = smmu->smrs ?
			arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(ctx->sme)) :
			0xffffffff;
		cbar = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBAR(ctx->cbndx));
		type = FIELD_GET(ARM_SMMU_S2CR_TYPE, s2cr);
		stream_cb = FIELD_GET(ARM_SMMU_S2CR_CBNDX, s2cr);

		a52_ackfr_record(
			"P276 278C0 q=%u s=%x e=%d c=%u a=%x",
			point, ctx->sid, ctx->sme, ctx->cbndx, actlr);
		a52_ackfr_record(
			"P276 278C1 q=%u s=%x x=%x m=%u f=%x",
			point, ctx->sid, sctlr,
			!!(sctlr & ARM_SMMU_SCTLR_M), fsr);
		a52_ackfr_record(
			"P276 278S0 q=%u s=%x z=%x t=%u x=%u",
			point, ctx->sid, s2cr, type, stream_cb);
		a52_ackfr_record(
			"P276 278S1 q=%u s=%x r=%x b=%x",
			point, ctx->sid, smr, cbar);

		arm_smmu_rpm_put(smmu);
	}

	if (!any)
		a52_ackfr_record("P276 278X q=%u n=1", point);
}
EXPORT_SYMBOL_GPL(a52_p278_display_smmu_snapshot);


/* A52_PHASE279_BROAD_DISPLAY_FAILURE_SNAPSHOT_V1
 * A52_PHASE280_PHASE279_EVIDENCE_VISIBLE_V1
 * Phase280 changes only diagnostic record formatting. The Phase278/279
 * SMMU reads, software IOVA walk, DSI call sites, ordering and control
 * flow are unchanged. Records use the existing admitted P276 namespace
 * and are split to fit the 73-byte persistent payload without truncation.
 * Broad but read-only cross-section at the already-proven DSI DMA failure
 * boundary. The command IOVA is translated through the software io-pgtable
 * walk only; no hardware translation request is issued by this recorder.
 * Context/global fault registers are read but never cleared here. TTBR0/TCR
 * are read back only to correlate the active hardware root with cached state.
 */
void a52_p279_display_iova_snapshot(unsigned int point,
		dma_addr_t iova, u32 len)
{
	bool any = false;
	int slot;

	for (slot = 0; slot < ARRAY_SIZE(a52_p278_display_ctx); slot++) {
		struct a52_p278_display_ctx *ctx = &a52_p278_display_ctx[slot];
		struct arm_smmu_device *smmu;
		struct arm_smmu_cb *cb;
		struct arm_smmu_cfg *cfg;
		struct arm_smmu_domain *smmu_domain;
		struct io_pgtable_ops *ops;
		dma_addr_t end = iova;
		phys_addr_t pa_first, pa_last;
		u64 hw_ttbr = 0, cached_ttbr;
		u32 hw_tcr = 0, cached_tcr;
		int ret;

		if (!ctx->valid)
			continue;
		any = true;
		smmu = ctx->smmu;
		if (!smmu || ctx->cbndx >= smmu->num_context_banks) {
			a52_ackfr_record("P276 279X q=%u s=%x r=2",
					  point, ctx->sid);
			continue;
		}

		cb = &smmu->cbs[ctx->cbndx];
		cfg = cb->cfg;
		if (!cfg) {
			a52_ackfr_record("P276 279X q=%u s=%x r=3",
					  point, ctx->sid);
			continue;
		}
		smmu_domain = cfg_to_smmu_domain(cfg);
		ops = smmu_domain->pgtbl_ops;
		if (!ops || !ops->iova_to_phys) {
			a52_ackfr_record("P276 279X q=%u s=%x r=4",
					  point, ctx->sid);
			continue;
		}

		if (len) {
			end = iova + (dma_addr_t)len - 1;
			if (end < iova) {
				a52_ackfr_record("P276 279X q=%u s=%x r=5",
						  point, ctx->sid);
				continue;
			}
		}

		pa_first = ops->iova_to_phys(ops, iova);
		pa_last = ops->iova_to_phys(ops, end);
		cached_ttbr = cb->ttbr[0];
		cached_tcr = cb->tcr[0];

		ret = arm_smmu_rpm_get(smmu);
		if (ret >= 0) {
			if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH32_S)
				hw_ttbr = arm_smmu_cb_read(smmu, ctx->cbndx,
							 ARM_SMMU_CB_TTBR0);
			else
				hw_ttbr = arm_smmu_cb_readq(smmu, ctx->cbndx,
							  ARM_SMMU_CB_TTBR0);
			hw_tcr = arm_smmu_cb_read(smmu, ctx->cbndx,
						 ARM_SMMU_CB_TCR);
			arm_smmu_rpm_put(smmu);
		}

		a52_ackfr_record(
			"P276 279I0 q=%u s=%x c=%u i=%llx n=%u",
			point, ctx->sid, ctx->cbndx,
			(unsigned long long)iova, len);
		a52_ackfr_record(
			"P276 279I1 q=%u s=%x e=%llx a=%llx",
			point, ctx->sid, (unsigned long long)end,
			(unsigned long long)pa_first);
		a52_ackfr_record(
			"P276 279I2 q=%u s=%x b=%llx",
			point, ctx->sid, (unsigned long long)pa_last);
		a52_ackfr_record(
			"P276 279T0 q=%u s=%x r=%d", point, ctx->sid, ret);
		a52_ackfr_record(
			"P276 279T1 q=%u s=%x h=%llx c=%llx",
			point, ctx->sid, (unsigned long long)hw_ttbr,
			(unsigned long long)cached_ttbr);
		a52_ackfr_record(
			"P276 279T2 q=%u s=%x h=%x c=%x",
			point, ctx->sid, hw_tcr, cached_tcr);
	}

	if (!any)
		a52_ackfr_record("P276 279X q=%u s=0 r=1", point);
}
EXPORT_SYMBOL_GPL(a52_p279_display_iova_snapshot);

void a52_p279_display_fault_snapshot(unsigned int point)
{
	struct arm_smmu_device *global_smmu = NULL;
	bool any = false;
	int slot;

	for (slot = 0; slot < ARRAY_SIZE(a52_p278_display_ctx); slot++) {
		struct a52_p278_display_ctx *ctx = &a52_p278_display_ctx[slot];
		struct arm_smmu_device *smmu;
		u32 fsr, fsynr0, cbfrsynra;
		u64 far;
		int ret;

		if (!ctx->valid)
			continue;
		any = true;
		smmu = ctx->smmu;
		if (!smmu || ctx->cbndx >= smmu->num_context_banks) {
			a52_ackfr_record("P276 279X q=%u s=%x r=6",
					  point, ctx->sid);
			continue;
		}

		ret = arm_smmu_rpm_get(smmu);
		if (ret < 0) {
			a52_ackfr_record("P276 279X q=%u s=%x r=7",
					  point, ctx->sid);
			continue;
		}

		fsr = arm_smmu_cb_read(smmu, ctx->cbndx, ARM_SMMU_CB_FSR);
		fsynr0 = arm_smmu_cb_read(smmu, ctx->cbndx, ARM_SMMU_CB_FSYNR0);
		far = arm_smmu_cb_readq(smmu, ctx->cbndx, ARM_SMMU_CB_FAR);
		cbfrsynra = arm_smmu_gr1_read(smmu,
					 ARM_SMMU_GR1_CBFRSYNRA(ctx->cbndx));
		a52_ackfr_record(
			"P276 279F0 q=%u s=%x c=%u f=%x y=%x",
			point, ctx->sid, ctx->cbndx, fsr, fsynr0);
		a52_ackfr_record(
			"P276 279F1 q=%u s=%x a=%llx r=%x",
			point, ctx->sid, (unsigned long long)far, cbfrsynra);

		if (global_smmu != smmu) {
			u32 gfsr, g0, g1, g2;

			gfsr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSR);
			g0 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR0);
			g1 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR1);
			g2 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSYNR2);
			a52_ackfr_record(
				"P276 279G0 q=%u f=%x a=%x b=%x",
				point, gfsr, g0, g1);
			a52_ackfr_record(
				"P276 279G1 q=%u c=%x", point, g2);
			global_smmu = smmu;
		}

		arm_smmu_rpm_put(smmu);
	}

	if (!any)
		a52_ackfr_record("P276 279X q=%u s=0 r=8", point);
}
EXPORT_SYMBOL_GPL(a52_p279_display_fault_snapshot);

/* A52_PHASE277_QSMMUV500_DISPLAY_ACTLR_PARITY_V1
 * Golden qcom,qsmmu-v500 parses qcom,actlr as SID/mask/ACTLR triples,
 * programs the matching context-bank ACTLR, then immediately flushes the
 * domain TLB.  Keep this experiment display-only: SID 0x800/0x801.
 */
static int a52_arm_smmu_apply_display_actlr(
		struct arm_smmu_domain *smmu_domain,
		struct iommu_fwspec *fwspec, struct device *dev)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct device_node *np = smmu->dev->of_node;
	u32 chosen_actlr = 0, chosen_sid = 0;
	u32 before, after;
	bool has_display_sid = false, matched = false;
	int count, i, j, ret;

	if (!a52_apps_smmu(smmu->dev) || !np)
		return 0;

	for (i = 0; i < fwspec->num_ids; i++) {
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);

		if (sid == 0x800 || sid == 0x801) {
			has_display_sid = true;
			break;
		}
	}
	if (!has_display_sid)
		return 0;

	count = of_property_count_u32_elems(np, "qcom,actlr");
	if (count < 0) {
		a52_ackfr_record("SMMU P277 ACTLR missing dev=%s rc=%d",
				  dev_name(dev), count);
		return count;
	}
	if (!count || count % 3) {
		a52_ackfr_record("SMMU P277 ACTLR malformed dev=%s cells=%d",
				  dev_name(dev), count);
		return -EINVAL;
	}

	for (i = 0; i < fwspec->num_ids; i++) {
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);
		u16 mask = FIELD_GET(ARM_SMMU_SMR_MASK, fwspec->ids[i]);

		if (sid != 0x800 && sid != 0x801)
			continue;

		for (j = 0; j < count; j += 3) {
			u32 table_sid, table_mask, table_actlr;

			ret = of_property_read_u32_index(np, "qcom,actlr", j,
							 &table_sid);
			if (ret)
				return ret;
			ret = of_property_read_u32_index(np, "qcom,actlr", j + 1,
							 &table_mask);
			if (ret)
				return ret;
			ret = of_property_read_u32_index(np, "qcom,actlr", j + 2,
							 &table_actlr);
			if (ret)
				return ret;

			if (table_sid > 0xffff || table_mask > 0xffff)
				return -EINVAL;

			/* Same overlap test used by Golden's SMR ACTLR matcher. */
			if ((sid ^ table_sid) & ~((u32)mask | table_mask))
				continue;

			if (matched && chosen_actlr != table_actlr) {
				a52_ackfr_record(
					"SMMU P277 ACTLR conflict sid=%x old=%x new=%x",
					sid, chosen_actlr, table_actlr);
				return -EINVAL;
			}

			matched = true;
			chosen_actlr = table_actlr;
			chosen_sid = sid;
		}
	}

	if (!matched || !chosen_actlr) {
		a52_ackfr_record("SMMU P277 ACTLR no-match dev=%s",
				  dev_name(dev));
		return -ENOENT;
	}
	if (smmu_domain->stage != ARM_SMMU_DOMAIN_S1 ||
	    !smmu_domain->flush_ops ||
	    !smmu_domain->flush_ops->tlb_flush_all)
		return -EINVAL;

	before = arm_smmu_cb_read(smmu, smmu_domain->cfg.cbndx,
				  ARM_SMMU_CB_ACTLR);
	arm_smmu_cb_write(smmu, smmu_domain->cfg.cbndx,
			  ARM_SMMU_CB_ACTLR, chosen_actlr);
	/* Golden ordering: ACTLR write, then full context/domain TLB flush. */
	smmu_domain->flush_ops->tlb_flush_all(smmu_domain);
	after = arm_smmu_cb_read(smmu, smmu_domain->cfg.cbndx,
				 ARM_SMMU_CB_ACTLR);

	a52_ackfr_record(
		"SMMU P277 ACTLR sid=%x cb=%u actlr=%x before=%x after=%x tlb=1",
		chosen_sid, smmu_domain->cfg.cbndx, chosen_actlr, before, after);

	if (after != chosen_actlr)
		return -EIO;

	return 0;
}

static int arm_smmu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_master_cfg *cfg;
	struct arm_smmu_device *smmu;
	int ret;

	if (!fwspec || fwspec->ops != &arm_smmu_ops) {
		dev_err(dev, "cannot attach to SMMU, is it on the same bus?\n");
		return -ENXIO;
	}

	/*
	 * FIXME: The arch/arm DMA API code tries to attach devices to its own
	 * domains between of_xlate() and probe_device() - we have no way to cope
	 * with that, so until ARM gets converted to rely on groups and default
	 * domains, just say no (but more politely than by dereferencing NULL).
	 * This should be at least a WARN_ON once that's sorted.
	 */
	cfg = dev_iommu_priv_get(dev);
	if (!cfg)
		return -ENODEV;

	smmu = cfg->smmu;
	a52_arm_smmu_apply_dt_domain_attrs(domain, smmu_domain, dev);

	ret = arm_smmu_rpm_get(smmu);
	if (ret < 0)
		return ret;

	/* Ensure that the domain is finalised */
	ret = arm_smmu_init_domain_context(domain, smmu, dev);
	if (ret < 0)
		goto rpm_put;

	if (a52_kgsl_dynamic_domain(domain)) {
		ret = 0;
		goto rpm_put;
	}

	/*
	 * Sanity check the domain. We don't support domains across
	 * different SMMUs.
	 */
	if (smmu_domain->smmu != smmu) {
		dev_err(dev,
			"cannot attach to SMMU %s whilst already attached to domain on SMMU %s\n",
			dev_name(smmu_domain->smmu->dev), dev_name(smmu->dev));
		ret = -EINVAL;
		goto rpm_put;
	}

	/* Looks ok, so add the device to the domain. Secure display uses the
	 * same translating stream path after its page tables are assigned to
	 * HLOS and the DT-provided secure VMID.
	 */
	ret = arm_smmu_domain_add_master(smmu_domain, cfg, fwspec);
	if (!ret) {
		a52_p278_remember_display_context(smmu_domain, fwspec, dev);
		ret = a52_arm_smmu_apply_display_actlr(smmu_domain, fwspec, dev);
	}

	/*
	 * Setup an autosuspend delay to avoid bouncing runpm state.
	 * Otherwise, if a driver for a suspended consumer device
	 * unmaps buffers, it will runpm resume/suspend for each one.
	 *
	 * For example, when used by a GPU device, when an application
	 * or game exits, it can trigger unmapping 100s or 1000s of
	 * buffers.  With a runpm cycle for each buffer, that adds up
	 * to 5-10sec worth of reprogramming the context bank, while
	 * the system appears to be locked up to the user.
	 */
	pm_runtime_set_autosuspend_delay(smmu->dev, 20);
	pm_runtime_use_autosuspend(smmu->dev);

rpm_put:
	arm_smmu_rpm_put(smmu);
	return ret;
}

static int arm_smmu_map_pages(struct iommu_domain *domain, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int prot, gfp_t gfp, size_t *mapped)
{
	struct io_pgtable_ops *ops = to_smmu_domain(domain)->pgtbl_ops;
	struct arm_smmu_device *smmu = to_smmu_domain(domain)->smmu;
	int ret;

	if (!ops)
		return -ENODEV;

	arm_smmu_rpm_get(smmu);
	arm_smmu_secure_domain_lock(to_smmu_domain(domain));
	ret = ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, gfp, mapped);
	{
		int assign_ret = arm_smmu_assign_table(to_smmu_domain(domain));

		/* A failed map may still allocate intermediate tables. Always
		 * transfer those pages, but preserve the original map failure.
		 */
		if (!ret)
			ret = assign_ret;
	}
	arm_smmu_secure_domain_unlock(to_smmu_domain(domain));
	arm_smmu_rpm_put(smmu);

	return ret;
}

static size_t arm_smmu_unmap_pages(struct iommu_domain *domain, unsigned long iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *iotlb_gather)
{
	struct io_pgtable_ops *ops = to_smmu_domain(domain)->pgtbl_ops;
	struct arm_smmu_device *smmu = to_smmu_domain(domain)->smmu;
	size_t ret;

	if (!ops)
		return 0;

	arm_smmu_rpm_get(smmu);
	arm_smmu_secure_domain_lock(to_smmu_domain(domain));
	ret = ops->unmap_pages(ops, iova, pgsize, pgcount, iotlb_gather);
	arm_smmu_assign_table(to_smmu_domain(domain));
	arm_smmu_unassign_table(to_smmu_domain(domain));
	arm_smmu_secure_domain_unlock(to_smmu_domain(domain));
	arm_smmu_rpm_put(smmu);

	return ret;
}

static void arm_smmu_flush_iotlb_all(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_device *smmu = smmu_domain->smmu;

	if (smmu_domain->flush_ops) {
		arm_smmu_rpm_get(smmu);
		smmu_domain->flush_ops->tlb_flush_all(smmu_domain);
		arm_smmu_rpm_put(smmu);
	}
}

static void arm_smmu_iotlb_sync(struct iommu_domain *domain,
				struct iommu_iotlb_gather *gather)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_device *smmu = smmu_domain->smmu;

	if (!smmu)
		return;

	arm_smmu_rpm_get(smmu);
	if (smmu->version == ARM_SMMU_V2 ||
	    smmu_domain->stage == ARM_SMMU_DOMAIN_S1)
		arm_smmu_tlb_sync_context(smmu_domain);
	else
		arm_smmu_tlb_sync_global(smmu);
	arm_smmu_rpm_put(smmu);
}

static phys_addr_t arm_smmu_iova_to_phys_hard(struct iommu_domain *domain,
					      dma_addr_t iova)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct io_pgtable_ops *ops= smmu_domain->pgtbl_ops;
	struct device *dev = smmu->dev;
	void __iomem *reg;
	u32 tmp;
	u64 phys;
	unsigned long va, flags;
	int ret, idx = cfg->cbndx;
	phys_addr_t addr = 0;

	ret = arm_smmu_rpm_get(smmu);
	if (ret < 0)
		return 0;

	spin_lock_irqsave(&smmu_domain->cb_lock, flags);
	va = iova & ~0xfffUL;
	if (cfg->fmt == ARM_SMMU_CTX_FMT_AARCH64)
		arm_smmu_cb_writeq(smmu, idx, ARM_SMMU_CB_ATS1PR, va);
	else
		arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_ATS1PR, va);

	reg = arm_smmu_page(smmu, ARM_SMMU_CB(smmu, idx)) + ARM_SMMU_CB_ATSR;
	if (readl_poll_timeout_atomic(reg, tmp, !(tmp & ARM_SMMU_ATSR_ACTIVE),
				      5, 50)) {
		spin_unlock_irqrestore(&smmu_domain->cb_lock, flags);
		dev_err(dev,
			"iova to phys timed out on %pad. Falling back to software table walk.\n",
			&iova);
		arm_smmu_rpm_put(smmu);
		return ops->iova_to_phys(ops, iova);
	}

	phys = arm_smmu_cb_readq(smmu, idx, ARM_SMMU_CB_PAR);
	spin_unlock_irqrestore(&smmu_domain->cb_lock, flags);
	if (phys & ARM_SMMU_CB_PAR_F) {
		dev_err(dev, "translation fault!\n");
		dev_err(dev, "PAR = 0x%llx\n", phys);
		goto out;
	}

	addr = (phys & GENMASK_ULL(39, 12)) | (iova & 0xfff);
out:
	arm_smmu_rpm_put(smmu);

	return addr;
}

static phys_addr_t arm_smmu_iova_to_phys(struct iommu_domain *domain,
					dma_addr_t iova)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct io_pgtable_ops *ops = smmu_domain->pgtbl_ops;

	if (domain->type == IOMMU_DOMAIN_IDENTITY)
		return iova;

	if (!ops)
		return 0;

	if (smmu_domain->smmu->features & ARM_SMMU_FEAT_TRANS_OPS &&
			smmu_domain->stage == ARM_SMMU_DOMAIN_S1)
		return arm_smmu_iova_to_phys_hard(domain, iova);

	return ops->iova_to_phys(ops, iova);
}

static bool arm_smmu_capable(enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		/*
		 * Return true here as the SMMU can always send out coherent
		 * requests.
		 */
		return true;
	case IOMMU_CAP_NOEXEC:
		return true;
	default:
		return false;
	}
}

static
struct arm_smmu_device *arm_smmu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev = driver_find_device_by_fwnode(&arm_smmu_driver.driver,
							  fwnode);
	put_device(dev);
	return dev ? dev_get_drvdata(dev) : NULL;
}

static struct iommu_device *arm_smmu_probe_device(struct device *dev)
{
	struct arm_smmu_device *smmu = NULL;
	struct arm_smmu_master_cfg *cfg;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	int i, ret;

	if (using_legacy_binding) {
		ret = arm_smmu_register_legacy_master(dev, &smmu);

		/*
		 * If dev->iommu_fwspec is initally NULL, arm_smmu_register_legacy_master()
		 * will allocate/initialise a new one. Thus we need to update fwspec for
		 * later use.
		 */
		fwspec = dev_iommu_fwspec_get(dev);
		if (ret)
			goto out_free;
	} else if (fwspec && fwspec->ops == &arm_smmu_ops) {
		smmu = arm_smmu_get_by_fwnode(fwspec->iommu_fwnode);

		/*
		 * Defer probe if the relevant SMMU instance hasn't finished
		 * probing yet. This is a fragile hack and we'd ideally
		 * avoid this race in the core code. Until that's ironed
		 * out, however, this is the most pragmatic option on the
		 * table.
		 */
		if (!smmu)
			return ERR_PTR(dev_err_probe(dev, -EPROBE_DEFER,
						"smmu dev has not bound yet\n"));
	} else {
		return ERR_PTR(-ENODEV);
	}

	ret = -EINVAL;
	for (i = 0; i < fwspec->num_ids; i++) {
		u16 sid = FIELD_GET(ARM_SMMU_SMR_ID, fwspec->ids[i]);
		u16 mask = FIELD_GET(ARM_SMMU_SMR_MASK, fwspec->ids[i]);

		if (sid & ~smmu->streamid_mask) {
			dev_err(dev, "stream ID 0x%x out of range for SMMU (0x%x)\n",
				sid, smmu->streamid_mask);
			goto out_free;
		}
		if (mask & ~smmu->smr_mask_mask) {
			dev_err(dev, "SMR mask 0x%x out of range for SMMU (0x%x)\n",
				mask, smmu->smr_mask_mask);
			goto out_free;
		}
	}

	ret = -ENOMEM;
	cfg = kzalloc(offsetof(struct arm_smmu_master_cfg, smendx[i]),
		      GFP_KERNEL);
	if (!cfg)
		goto out_free;

	cfg->smmu = smmu;
	dev_iommu_priv_set(dev, cfg);
	while (i--)
		cfg->smendx[i] = INVALID_SMENDX;

	ret = arm_smmu_rpm_get(smmu);
	if (ret < 0)
		goto out_cfg_free;

	ret = arm_smmu_master_alloc_smes(dev);
	arm_smmu_rpm_put(smmu);

	if (ret)
		goto out_cfg_free;

	device_link_add(dev, smmu->dev,
			DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER);

	return &smmu->iommu;

out_cfg_free:
	kfree(cfg);
out_free:
	iommu_fwspec_free(dev);
	return ERR_PTR(ret);
}

static void arm_smmu_release_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_master_cfg *cfg;
	struct arm_smmu_device *smmu;
	int ret;

	if (!fwspec || fwspec->ops != &arm_smmu_ops)
		return;

	cfg  = dev_iommu_priv_get(dev);
	smmu = cfg->smmu;

	ret = arm_smmu_rpm_get(smmu);
	if (ret < 0)
		return;

	arm_smmu_master_free_smes(cfg, fwspec);

	arm_smmu_rpm_put(smmu);

	dev_iommu_priv_set(dev, NULL);
	kfree(cfg);
	iommu_fwspec_free(dev);
}

static struct iommu_group *arm_smmu_device_group(struct device *dev)
{
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_device *smmu = cfg->smmu;
	struct iommu_group *group = NULL;
	int i, idx;

	for_each_cfg_sme(cfg, fwspec, i, idx) {
		if (group && smmu->s2crs[idx].group &&
		    group != smmu->s2crs[idx].group)
			return ERR_PTR(-EINVAL);

		group = smmu->s2crs[idx].group;
	}

	if (group)
		return iommu_group_ref_get(group);

	if (dev_is_pci(dev))
		group = pci_device_group(dev);
	else if (dev_is_fsl_mc(dev))
		group = fsl_mc_device_group(dev);
	else
		group = generic_device_group(dev);

	/* Remember group for faster lookups */
	if (!IS_ERR(group))
		for_each_cfg_sme(cfg, fwspec, i, idx)
			smmu->s2crs[idx].group = group;

	return group;
}

static int arm_smmu_enable_s1_translations(
		struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	u32 reg;
	int ret;

	if (!smmu)
		return 0;

	ret = arm_smmu_rpm_get(smmu);
	if (ret < 0)
		return ret;

	reg = arm_smmu_cb_read(smmu, smmu_domain->cfg.cbndx,
			       ARM_SMMU_CB_SCTLR);
	reg |= ARM_SMMU_SCTLR_M;
	arm_smmu_cb_write(smmu, smmu_domain->cfg.cbndx,
			  ARM_SMMU_CB_SCTLR, reg);
	arm_smmu_rpm_put(smmu);

	return 0;
}

static int arm_smmu_domain_get_attr(struct iommu_domain *domain,
				    enum iommu_attr attr, void *data)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;

	switch (attr) {
	case DOMAIN_ATTR_EARLY_MAP:
	case DOMAIN_ATTR_NON_FATAL_FAULTS:
		*(int *)data = !!(smmu_domain->attributes & BIT(attr));
		return 0;
	case DOMAIN_ATTR_SECURE_VMID:
		*(int *)data = smmu_domain->secure_vmid;
		return 0;
	case DOMAIN_ATTR_PROCID:
		*(u32 *)data = cfg->procid;
		return 0;
	case DOMAIN_ATTR_DYNAMIC:
		*(int *)data = a52_kgsl_dynamic_domain(domain);
		return 0;
	case DOMAIN_ATTR_CONTEXT_BANK:
		if (!smmu_domain->smmu)
			return -ENODEV;
		*(unsigned int *)data = cfg->cbndx;
		return 0;
	case DOMAIN_ATTR_TTBR0: {
		u64 val;

		if (!smmu_domain->smmu || !smmu_domain->pgtbl_ops)
			return -ENODEV;
		if (smmu_domain->stage != ARM_SMMU_DOMAIN_S1)
			return -EINVAL;

		val = smmu_domain->pgtbl_cfg.arm_lpae_s1_cfg.ttbr;
		val |= FIELD_PREP(ARM_SMMU_TTBRn_ASID, cfg->asid);
		*(u64 *)data = val;
		return 0;
	}
	case DOMAIN_ATTR_CONTEXTIDR:
		if (!smmu_domain->smmu)
			return -ENODEV;
		*(u32 *)data = cfg->procid;
		return 0;
	default:
		break;
	}

	switch (domain->type) {
	case IOMMU_DOMAIN_UNMANAGED:
		switch (attr) {
		case DOMAIN_ATTR_NESTING:
			*(int *)data = (smmu_domain->stage == ARM_SMMU_DOMAIN_NESTED);
			return 0;
		default:
			return -ENODEV;
		}
	case IOMMU_DOMAIN_DMA:
		switch (attr) {
		case DOMAIN_ATTR_DMA_USE_FLUSH_QUEUE:
			*(int *)data = smmu_domain->non_strict;
			return 0;
		default:
			return -ENODEV;
		}
	default:
		return -EINVAL;
	}
}


static int arm_smmu_domain_set_attr(struct iommu_domain *domain,
				    enum iommu_attr attr, void *data)
{
	int ret = 0;
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;

	mutex_lock(&smmu_domain->init_mutex);

	switch (attr) {
	case DOMAIN_ATTR_EARLY_MAP:
		if (*(int *)data) {
			smmu_domain->attributes |= BIT(DOMAIN_ATTR_EARLY_MAP);
		} else {
			ret = arm_smmu_enable_s1_translations(smmu_domain);
			if (!ret)
				smmu_domain->attributes &= ~BIT(DOMAIN_ATTR_EARLY_MAP);
		}
		goto out_unlock;
	case DOMAIN_ATTR_NON_FATAL_FAULTS:
		if (*(int *)data)
			smmu_domain->attributes |= BIT(DOMAIN_ATTR_NON_FATAL_FAULTS);
		else
			smmu_domain->attributes &= ~BIT(DOMAIN_ATTR_NON_FATAL_FAULTS);
		goto out_unlock;
	case DOMAIN_ATTR_SECURE_VMID:
		if (smmu_domain->smmu)
			ret = -EBUSY;
		else if (smmu_domain->secure_vmid != VMID_INVAL)
			ret = -EEXIST;
		else
			smmu_domain->secure_vmid = *(int *)data;
		goto out_unlock;
	case DOMAIN_ATTR_PROCID:
		if (smmu_domain->smmu) {
			ret = -EBUSY;
		} else {
			cfg->procid = *(u32 *)data;
			smmu_domain->attributes |= BIT(DOMAIN_ATTR_PROCID);
		}
		goto out_unlock;
	case DOMAIN_ATTR_DYNAMIC:
		if (smmu_domain->smmu) {
			ret = -EBUSY;
		} else if (*(int *)data) {
			smmu_domain->attributes |= BIT(DOMAIN_ATTR_DYNAMIC);
		} else {
			smmu_domain->attributes &= ~BIT(DOMAIN_ATTR_DYNAMIC);
		}
		goto out_unlock;
	case DOMAIN_ATTR_CONTEXT_BANK:
		if (smmu_domain->smmu) {
			ret = -EBUSY;
		} else if (!a52_kgsl_dynamic_domain(domain)) {
			ret = -EINVAL;
		} else {
			cfg->cbndx = *(unsigned int *)data;
		}
		goto out_unlock;
	default:
		break;
	}

	switch (domain->type) {
	case IOMMU_DOMAIN_UNMANAGED:
		switch (attr) {
		case DOMAIN_ATTR_NESTING:
			if (smmu_domain->smmu) {
				ret = -EPERM;
				goto out_unlock;
			}
			if (*(int *)data)
				smmu_domain->stage = ARM_SMMU_DOMAIN_NESTED;
			else
				smmu_domain->stage = ARM_SMMU_DOMAIN_S1;
			break;
		default:
			ret = -ENODEV;
		}
		break;
	case IOMMU_DOMAIN_DMA:
		switch (attr) {
		case DOMAIN_ATTR_DMA_USE_FLUSH_QUEUE:
			smmu_domain->non_strict = *(int *)data;
			break;
		default:
			ret = -ENODEV;
		}
		break;
	default:
		ret = -EINVAL;
	}

out_unlock:
	mutex_unlock(&smmu_domain->init_mutex);
	return ret;
}


static int arm_smmu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	u32 mask, fwid = 0;

	if (args->args_count > 0)
		fwid |= FIELD_PREP(ARM_SMMU_SMR_ID, args->args[0]);

	if (args->args_count > 1)
		fwid |= FIELD_PREP(ARM_SMMU_SMR_MASK, args->args[1]);
	else if (!of_property_read_u32(args->np, "stream-match-mask", &mask))
		fwid |= FIELD_PREP(ARM_SMMU_SMR_MASK, mask);

	return iommu_fwspec_add_ids(dev, &fwid, 1);
}


/* P470_A2_LAGOON_IPA_DMA_POOL_COMPLEMENT_V2
 * qcom,iommu-dma-addr-pool is the DMA allocator's USABLE range.
 * Return IOMMU_RESV_RESERVED for the COMPLEMENT of that window, not for
 * the window itself. Linux 5.10's iommu_dma_init_domain() applies these
 * reservations to its IOVA allocator, while IOMMU_RESV_DIRECT creates
 * the fixed identity mapping via iommu_create_device_direct_mappings().
 *
 * Scope exactly to the three unmasked Lagoon IPA stream IDs. Keep the
 * Phase206/P460B system-suspend and SMR/SME code unchanged.
 */
static bool p470_a2_ipa_append_resv(struct device *dev,
                                    struct list_head *head,
                                    phys_addr_t base, size_t size,
                                    enum iommu_resv_type type)
{
    struct iommu_resv_region *region;
    int prot = IOMMU_READ | IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;

    if (!size)
        return true;
    region = iommu_alloc_resv_region(base, size, prot, type);
    if (!region) {
        dev_err(dev, "P470 IPA SMMU reservation allocation failed base=%pa size=%zu\n",
                &base, size);
        return false;
    }
    list_add_tail(&region->list, head);
    return true;
}

/* All ends are INCLUSIVE. Never form aperture_end+1 because the domain
 * can span the full 64-bit address width. Pool_end is EXCLUSIVE.
 */
static bool p470_a2_ipa_reserve_complement(struct device *dev,
                                            struct list_head *head,
                                            const char *cb,
                                            struct iommu_domain *domain,
                                            u64 pool_start, u64 pool_size,
                                            u64 ap_limit_exclusive)
{
    u64 lo, hi, pool_end;

    if (!domain || domain->type != IOMMU_DOMAIN_DMA ||
        !domain->geometry.force_aperture) {
        dev_err(dev, "P470 IPA SMMU cb=%s DMA aperture unavailable\n", cb);
        return false;
    }
    lo = domain->geometry.aperture_start;
    hi = domain->geometry.aperture_end;
    /* Keep the *full* hardware aperture for the upper reservation.
     * AP geometry-mapping is a tighter policy cap, not a reason to leave
     * the IOVAs above 0xF0000000 available with a 64-bit DMA mask.
     */
    pool_end = pool_start + pool_size;
    if (!pool_size || pool_end <= pool_start ||
        pool_start < lo || pool_end - 1 > hi ||
        (ap_limit_exclusive && pool_end > ap_limit_exclusive)) {
        dev_err(dev,
                "P470 IPA SMMU cb=%s invalid pool/aperture [%llx,%llx] pool=%llx+%llx\n",
                cb, lo, hi, pool_start, pool_size);
        return false;
    }

    /* Reservation of [lo, pool_start) and [pool_end, hi] (inclusive).
     * The first byte of the pool and its last byte are NEVER reserved.
 * AP's full 64-bit aperture above geometry_end is RESERVED too.
     */
    if (lo < pool_start &&
        !p470_a2_ipa_append_resv(dev, head, lo, pool_start - lo,
                                 IOMMU_RESV_RESERVED))
        return false;
    if (pool_end <= hi &&
        !p470_a2_ipa_append_resv(dev, head, pool_end,
                                 hi - pool_end + 1,
                                 IOMMU_RESV_RESERVED))
        return false;

    dev_info(dev,
             "P470 IPA SMMU cb=%s dma_pool=0x%llx+0x%llx outside_reserved=1 aperture=[0x%llx..0x%llx]\n",
             cb, pool_start, pool_size, lo, hi);
    return true;
}

static void p470_a2_lagoon_ipa_resv(struct device *dev,
                                    struct list_head *head)
{
    const struct iommu_fwspec *fw;
    struct iommu_domain *domain;
    const char *cb;
    u32 sid, mask, expected;
    u32 pool[2], geom[2];
    u64 pool_start = 0, pool_size = 0, limit = 0;
    u64 direct_start = 0, direct_size = 0;
    int ret;

    if (!dev || !dev->of_node)
        return;
    if (of_device_is_compatible(dev->of_node, "qcom,ipa-smmu-ap-cb")) {
        cb = "ap";
        expected = 0x440;
        pool_start = 0x20000000ULL;
        pool_size = 0x40000000ULL;
        limit = 0xF0000000ULL; /* TG qcom,geometry-mapping [0, 0xF0000000) */
        direct_start = 0x146a8000;
        direct_size = 0x2000;
    } else if (of_device_is_compatible(dev->of_node,
                                       "qcom,ipa-smmu-wlan-cb")) {
        cb = "wlan";
        expected = 0x441;
        direct_start = 0x01ea0000;
        direct_size = 0x80000;
    } else if (of_device_is_compatible(dev->of_node,
                                       "qcom,ipa-smmu-uc-cb")) {
        cb = "uc";
        expected = 0x442;
        pool_start = 0x40000000ULL;
        pool_size = 0x20000000ULL;
    } else {
        return;
    }

    fw = dev_iommu_fwspec_get(dev);
    if (!fw || fw->num_ids != 1) {
        dev_err(dev, "P470 IPA SMMU cb=%s invalid fwspec; reservations unavailable\n",
                cb);
        return;
    }
    sid = FIELD_GET(ARM_SMMU_SMR_ID, fw->ids[0]);
    mask = FIELD_GET(ARM_SMMU_SMR_MASK, fw->ids[0]);
    if (sid != expected || mask) {
        dev_err(dev,
                "P470 IPA SMMU cb=%s SID mismatch sid=0x%x mask=0x%x expected=0x%x\n",
                cb, sid, mask, expected);
        return;
    }

    /* The pool belongs to DMA. Reject changed DT sizes or an unexpected
     * AP geometry rather than letting an unscoped reservation run.
     */
    if (pool_size) {
        ret = of_property_read_u32_array(dev->of_node,
                                          "qcom,iommu-dma-addr-pool",
                                          pool, ARRAY_SIZE(pool));
        if (ret || pool[0] != pool_start || pool[1] != pool_size ||
            of_property_count_u32_elems(dev->of_node,
                                         "qcom,iommu-dma-addr-pool") != 2) {
            dev_err(dev, "P470 IPA SMMU cb=%s DMA pool DT mismatch\n", cb);
            return;
        }
    }
    if (sid == 0x440) {
        ret = of_property_read_u32_array(dev->of_node,
                                          "qcom,geometry-mapping",
                                          geom, ARRAY_SIZE(geom));
        if (ret || geom[0] != 0 || geom[1] != limit ||
            of_property_count_u32_elems(dev->of_node,
                                         "qcom,geometry-mapping") != 2) {
            dev_err(dev, "P470 IPA SMMU cb=ap geometry DT mismatch\n");
            return;
        }
    }

    domain = iommu_get_domain_for_dev(dev);
    if (pool_size &&
        !p470_a2_ipa_reserve_complement(dev, head, cb, domain,
                                         pool_start, pool_size, limit))
        return;

    /* Existing TG fixed windows must be preserved DIRECT and identity.
     * These windows are inside the reserved complement, but are mapped
     * once by the IOMMU core, NOT blindly remapped by IPA.
     */
    if (direct_size &&
        !p470_a2_ipa_append_resv(dev, head, direct_start, direct_size,
                                 IOMMU_RESV_DIRECT))
        return;

    dev_info(dev, "P470 IPA SMMU cb=%s sid=0x%03x DIRECT window ready for IOMMU core\n",
             cb, sid);
}

static void arm_smmu_get_resv_regions(struct device *dev,
				      struct list_head *head)
{
	struct iommu_resv_region *region;
	int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;

	region = iommu_alloc_resv_region(MSI_IOVA_BASE, MSI_IOVA_LENGTH,
					 prot, IOMMU_RESV_SW_MSI);
	if (!region)
		return;

	list_add_tail(&region->list, head);

	p470_a2_lagoon_ipa_resv(dev, head);
	iommu_dma_get_resv_regions(dev, head);
}

static int arm_smmu_def_domain_type(struct device *dev)
{
	struct arm_smmu_master_cfg *cfg = dev_iommu_priv_get(dev);
	const struct arm_smmu_impl *impl = cfg->smmu->impl;

	if (impl && impl->def_domain_type)
		return impl->def_domain_type(dev);

	return 0;
}

static struct iommu_ops arm_smmu_ops = {
	.capable		= arm_smmu_capable,
	.domain_alloc		= arm_smmu_domain_alloc,
	.domain_free		= arm_smmu_domain_free,
	.attach_dev		= arm_smmu_attach_dev,
	.map_pages		= arm_smmu_map_pages,
	.unmap_pages		= arm_smmu_unmap_pages,
	.flush_iotlb_all	= arm_smmu_flush_iotlb_all,
	.iotlb_sync		= arm_smmu_iotlb_sync,
	.iova_to_phys		= arm_smmu_iova_to_phys,
	.probe_device		= arm_smmu_probe_device,
	.release_device		= arm_smmu_release_device,
	.device_group		= arm_smmu_device_group,
	.domain_get_attr	= arm_smmu_domain_get_attr,
	.domain_set_attr	= arm_smmu_domain_set_attr,
	.of_xlate		= arm_smmu_of_xlate,
	.get_resv_regions	= arm_smmu_get_resv_regions,
	.put_resv_regions	= generic_iommu_put_resv_regions,
	.def_domain_type	= arm_smmu_def_domain_type,
	.pgsize_bitmap		= -1UL, /* Restricted during device attach */
};

/* A52_PHASE336_QSMMUV500_HF0_TBU_STATE_RECORDER_V1
 * Preserve the early apps-SMMU Phase335 result and expose a read-only
 * late snapshot for the exact F0 DSI recorder.
 *
 * The physical addresses are not guessed:
 *   apps_smmu       = 0x15000000
 *   mnoc_hf_0 TBU   = 0x1518d000
 *   HF0 status-reg  = 0x15182210
 * from the pinned lito msm-arm-smmu DT. SID 0x800 belongs to HF0.
 *
 * No SMMU/TBU register is written by Phase336.
 */
static u32 a52_p336_sacr_before;
static u32 a52_p336_sacr_after;
static bool a52_p336_sacr_seen;
static void __iomem *a52_p336_apps_smmu_base;
static void __iomem *a52_p336_hf0_status;

void a52_p336_qsmmuv500_snapshot(u32 *seen, u32 *sacr_before,
					 u32 *sacr_after, u32 *sacr_current,
					 u32 *tbu_valid, u32 *tbu_status)
{
	void __iomem *smmu_base = READ_ONCE(a52_p336_apps_smmu_base);
	void __iomem *hf0_status = READ_ONCE(a52_p336_hf0_status);

	if (seen)
		*seen = READ_ONCE(a52_p336_sacr_seen);
	if (sacr_before)
		*sacr_before = READ_ONCE(a52_p336_sacr_before);
	if (sacr_after)
		*sacr_after = READ_ONCE(a52_p336_sacr_after);
	if (sacr_current)
		*sacr_current = smmu_base ?
			readl_relaxed(smmu_base + 0x10) : 0xffffffff;
	if (tbu_valid)
		*tbu_valid = !!hf0_status;
	if (tbu_status)
		*tbu_status = hf0_status ? readl_relaxed(hf0_status) : 0xffffffff;
}
EXPORT_SYMBOL_GPL(a52_p336_qsmmuv500_snapshot);

static void arm_smmu_device_reset(struct arm_smmu_device *smmu)
{
	int i;
	u32 reg;

	/* clear global FSR */
	reg = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSR);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sGFSR, reg);

	/*
	 * Preserve firmware stream mappings and context banks when the
	 * downstream DT explicitly declares qcom,skip-init.
	 */
	if (!smmu->skip_init) {
		for (i = 0; i < smmu->num_mapping_groups; ++i)
			arm_smmu_write_sme(smmu, i);

		/* Disable context banks and clear their fault status. */
		for (i = 0; i < smmu->num_context_banks; ++i) {
			arm_smmu_write_context_bank(smmu, i);
			arm_smmu_cb_write(smmu, i, ARM_SMMU_CB_FSR,
					  ARM_SMMU_FSR_FAULT);
		}
	}

	/* Invalidate the TLB, just in case */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIALLH, QCOM_DUMMY_VAL);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIALLNSNH, QCOM_DUMMY_VAL);

	reg = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sCR0);

	/* Enable fault reporting */
	reg |= (ARM_SMMU_sCR0_GFRE | ARM_SMMU_sCR0_GFIE |
		ARM_SMMU_sCR0_GCFGFRE | ARM_SMMU_sCR0_GCFGFIE);

	/* Disable TLB broadcasting. */
	reg |= (ARM_SMMU_sCR0_VMIDPNE | ARM_SMMU_sCR0_PTM);

	/* Enable client access, handling unmatched streams as appropriate */
	reg &= ~ARM_SMMU_sCR0_CLIENTPD;
	if (disable_bypass)
		reg |= ARM_SMMU_sCR0_USFCFG;
	else
		reg &= ~ARM_SMMU_sCR0_USFCFG;

	/* Disable forced broadcasting */
	reg &= ~ARM_SMMU_sCR0_FB;

	/* Don't upgrade barriers */
	reg &= ~(ARM_SMMU_sCR0_BSU);

	if (smmu->features & ARM_SMMU_FEAT_VMID16)
		reg |= ARM_SMMU_sCR0_VMID16EN;

	if (smmu->features & ARM_SMMU_FEAT_EXIDS)
		reg |= ARM_SMMU_sCR0_EXIDENABLE;

	/* A52_PHASE335_QSMMUV500_SACR_CACHE_LOCK_PARITY_V1
	 * TouchGrass QSMMUv500 init clears MMU-500 sACR.CACHE_LOCK even
	 * when firmware-owned stream/context state is preserved. Our GKI
	 * qcom,skip-init path suppresses the Qualcomm implementation reset,
	 * so reproduce only that one TouchGrass register semantic here.
	 *
	 * Scope is deliberately narrow:
	 *   - qcom,qsmmu-v500 only
	 *   - qcom,skip-init only
	 *   - sACR offset 0x10 only
	 *   - CACHE_LOCK bit 26 only
	 * No stream-map, context-bank, TLB, ACTLR or reset behavior changes.
	 */
	if (smmu->skip_init &&
	    of_device_is_compatible(smmu->dev->of_node, "qcom,qsmmu-v500")) {
		u32 a52_p335_before;
		u32 a52_p335_wanted;
		u32 a52_p335_after;

		a52_p335_before = readl_relaxed(smmu->base + 0x10);
		a52_p335_wanted = a52_p335_before & ~(1U << 26);
		if (a52_p335_wanted != a52_p335_before)
			writel_relaxed(a52_p335_wanted, smmu->base + 0x10);
		a52_p335_after = readl_relaxed(smmu->base + 0x10);

		/*
		 * Phase336: retain the Phase335 result only for apps_smmu and
		 * map only the HF0 TBU status register used by SID 0x800.
		 * The managed MMIO helper creates a read-only-observer mapping here; no
		 * write, power vote, reset, delay or transaction is issued.
		 */
		{
			struct platform_device *a52_p336_pdev =
				to_platform_device(smmu->dev);
			struct resource *a52_p336_res =
				platform_get_resource(a52_p336_pdev, IORESOURCE_MEM, 0);

			if (a52_p336_res &&
			    a52_p336_res->start == (resource_size_t)0x15000000) {
				void __iomem *a52_p336_status;

				WRITE_ONCE(a52_p336_sacr_before, a52_p335_before);
				WRITE_ONCE(a52_p336_sacr_after, a52_p335_after);
				WRITE_ONCE(a52_p336_apps_smmu_base, smmu->base);
				WRITE_ONCE(a52_p336_sacr_seen, true);

				a52_p336_status = READ_ONCE(a52_p336_hf0_status);
				if (!a52_p336_status) {
					a52_p336_status = devm_ioremap(smmu->dev,
						(resource_size_t)0x15182210, 0x8);
					if (a52_p336_status)
						WRITE_ONCE(a52_p336_hf0_status,
							   a52_p336_status);
				}
			}
		}

		dev_info(smmu->dev,
			 "A52 P335 QSMMUV500 sACR before=%08x after=%08x changed=%u lock=%u\n",
			 a52_p335_before, a52_p335_after,
			 a52_p335_before != a52_p335_after,
			 !!(a52_p335_after & (1U << 26)));
	}

	if (!smmu->skip_init && smmu->impl && smmu->impl->reset)
		smmu->impl->reset(smmu);

	/* Push the button */
	arm_smmu_tlb_sync_global(smmu);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, reg);
}

static int arm_smmu_id_size_to_bits(int size)
{
	switch (size) {
	case 0:
		return 32;
	case 1:
		return 36;
	case 2:
		return 40;
	case 3:
		return 42;
	case 4:
		return 44;
	case 5:
	default:
		return 48;
	}
}

static int arm_smmu_device_cfg_probe(struct arm_smmu_device *smmu)
{
	unsigned int size;
	u32 id;
	bool cttw_reg, cttw_fw = smmu->features & ARM_SMMU_FEAT_COHERENT_WALK;
	int i, ret;

	dev_notice(smmu->dev, "probing hardware configuration...\n");
	dev_notice(smmu->dev, "SMMUv%d with:\n",
			smmu->version == ARM_SMMU_V2 ? 2 : 1);

	/* ID0 */
	id = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID0);

	/* Restrict available stages based on module parameter */
	if (force_stage == 1)
		id &= ~(ARM_SMMU_ID0_S2TS | ARM_SMMU_ID0_NTS);
	else if (force_stage == 2)
		id &= ~(ARM_SMMU_ID0_S1TS | ARM_SMMU_ID0_NTS);

	if (id & ARM_SMMU_ID0_S1TS) {
		smmu->features |= ARM_SMMU_FEAT_TRANS_S1;
		dev_notice(smmu->dev, "\tstage 1 translation\n");
	}

	if (id & ARM_SMMU_ID0_S2TS) {
		smmu->features |= ARM_SMMU_FEAT_TRANS_S2;
		dev_notice(smmu->dev, "\tstage 2 translation\n");
	}

	if (id & ARM_SMMU_ID0_NTS) {
		smmu->features |= ARM_SMMU_FEAT_TRANS_NESTED;
		dev_notice(smmu->dev, "\tnested translation\n");
	}

	if (!(smmu->features &
		(ARM_SMMU_FEAT_TRANS_S1 | ARM_SMMU_FEAT_TRANS_S2))) {
		dev_err(smmu->dev, "\tno translation support!\n");
		return -ENODEV;
	}

	if ((id & ARM_SMMU_ID0_S1TS) &&
	    ((smmu->version < ARM_SMMU_V2) || !(id & ARM_SMMU_ID0_ATOSNS))) {
		smmu->features |= ARM_SMMU_FEAT_TRANS_OPS;
		dev_notice(smmu->dev, "\taddress translation ops\n");
	}

	/*
	 * In order for DMA API calls to work properly, we must defer to what
	 * the FW says about coherency, regardless of what the hardware claims.
	 * Fortunately, this also opens up a workaround for systems where the
	 * ID register value has ended up configured incorrectly.
	 */
	cttw_reg = !!(id & ARM_SMMU_ID0_CTTW);
	if (cttw_fw || cttw_reg)
		dev_notice(smmu->dev, "\t%scoherent table walk\n",
			   cttw_fw ? "" : "non-");
	if (cttw_fw != cttw_reg)
		dev_notice(smmu->dev,
			   "\t(IDR0.CTTW overridden by FW configuration)\n");

	/* Max. number of entries we have for stream matching/indexing */
	if (smmu->version == ARM_SMMU_V2 && id & ARM_SMMU_ID0_EXIDS) {
		smmu->features |= ARM_SMMU_FEAT_EXIDS;
		size = 1 << 16;
	} else {
		size = 1 << FIELD_GET(ARM_SMMU_ID0_NUMSIDB, id);
	}
	smmu->streamid_mask = size - 1;
	if (id & ARM_SMMU_ID0_SMS) {
		smmu->features |= ARM_SMMU_FEAT_STREAM_MATCH;
		size = FIELD_GET(ARM_SMMU_ID0_NUMSMRG, id);
		if (size == 0) {
			dev_err(smmu->dev,
				"stream-matching supported, but no SMRs present!\n");
			return -ENODEV;
		}

		/* Zero-initialised to mark as invalid */
		smmu->smrs = devm_kcalloc(smmu->dev, size, sizeof(*smmu->smrs),
					  GFP_KERNEL);
		if (!smmu->smrs)
			return -ENOMEM;

		dev_notice(smmu->dev,
			   "\tstream matching with %u register groups", size);
	}
	/* s2cr->type == 0 means translation, so initialise explicitly */
	smmu->s2crs = devm_kmalloc_array(smmu->dev, size, sizeof(*smmu->s2crs),
					 GFP_KERNEL);
	if (!smmu->s2crs)
		return -ENOMEM;
	for (i = 0; i < size; i++)
		smmu->s2crs[i] = s2cr_init_val;

	smmu->num_mapping_groups = size;
	mutex_init(&smmu->stream_map_mutex);
	spin_lock_init(&smmu->global_sync_lock);

	if (smmu->version < ARM_SMMU_V2 ||
	    !(id & ARM_SMMU_ID0_PTFS_NO_AARCH32)) {
		smmu->features |= ARM_SMMU_FEAT_FMT_AARCH32_L;
		if (!(id & ARM_SMMU_ID0_PTFS_NO_AARCH32S))
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH32_S;
	}

	/* ID1 */
	id = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID1);
	smmu->pgshift = (id & ARM_SMMU_ID1_PAGESIZE) ? 16 : 12;

	/* Check for size mismatch of SMMU address space from mapped region */
	size = 1 << (FIELD_GET(ARM_SMMU_ID1_NUMPAGENDXB, id) + 1);
	if (smmu->numpage != 2 * size << smmu->pgshift)
		dev_warn(smmu->dev,
			"SMMU address space size (0x%x) differs from mapped region size (0x%x)!\n",
			2 * size << smmu->pgshift, smmu->numpage);
	/* Now properly encode NUMPAGE to subsequently derive SMMU_CB_BASE */
	smmu->numpage = size;

	smmu->num_s2_context_banks = FIELD_GET(ARM_SMMU_ID1_NUMS2CB, id);
	smmu->num_context_banks = FIELD_GET(ARM_SMMU_ID1_NUMCB, id);
	if (smmu->num_s2_context_banks > smmu->num_context_banks) {
		dev_err(smmu->dev, "impossible number of S2 context banks!\n");
		return -ENODEV;
	}
	dev_notice(smmu->dev, "\t%u context banks (%u stage-2 only)\n",
		   smmu->num_context_banks, smmu->num_s2_context_banks);
	smmu->cbs = devm_kcalloc(smmu->dev, smmu->num_context_banks,
				 sizeof(*smmu->cbs), GFP_KERNEL);
	if (!smmu->cbs)
		return -ENOMEM;

	/* ID2 */
	id = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID2);
	size = arm_smmu_id_size_to_bits(FIELD_GET(ARM_SMMU_ID2_IAS, id));
	smmu->ipa_size = size;

	/* The output mask is also applied for bypass */
	size = arm_smmu_id_size_to_bits(FIELD_GET(ARM_SMMU_ID2_OAS, id));
	smmu->pa_size = size;

	if (id & ARM_SMMU_ID2_VMID16)
		smmu->features |= ARM_SMMU_FEAT_VMID16;

	/*
	 * What the page table walker can address actually depends on which
	 * descriptor format is in use, but since a) we don't know that yet,
	 * and b) it can vary per context bank, this will have to do...
	 */
	if (dma_set_mask_and_coherent(smmu->dev, DMA_BIT_MASK(size)))
		dev_warn(smmu->dev,
			 "failed to set DMA mask for table walker\n");

	if (smmu->version < ARM_SMMU_V2) {
		smmu->va_size = smmu->ipa_size;
		if (smmu->version == ARM_SMMU_V1_64K)
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH64_64K;
	} else {
		size = FIELD_GET(ARM_SMMU_ID2_UBS, id);
		smmu->va_size = arm_smmu_id_size_to_bits(size);
		if (id & ARM_SMMU_ID2_PTFS_4K)
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH64_4K;
		if (id & ARM_SMMU_ID2_PTFS_16K)
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH64_16K;
		if (id & ARM_SMMU_ID2_PTFS_64K)
			smmu->features |= ARM_SMMU_FEAT_FMT_AARCH64_64K;
	}

	if (smmu->impl && smmu->impl->cfg_probe) {
		ret = smmu->impl->cfg_probe(smmu);
		if (ret)
			return ret;
	}

	/* Now we've corralled the various formats, what'll it do? */
	if (smmu->features & ARM_SMMU_FEAT_FMT_AARCH32_S)
		smmu->pgsize_bitmap |= SZ_4K | SZ_64K | SZ_1M | SZ_16M;
	if (smmu->features &
	    (ARM_SMMU_FEAT_FMT_AARCH32_L | ARM_SMMU_FEAT_FMT_AARCH64_4K))
		smmu->pgsize_bitmap |= SZ_4K | SZ_2M | SZ_1G;
	if (smmu->features & ARM_SMMU_FEAT_FMT_AARCH64_16K)
		smmu->pgsize_bitmap |= SZ_16K | SZ_32M;
	if (smmu->features & ARM_SMMU_FEAT_FMT_AARCH64_64K)
		smmu->pgsize_bitmap |= SZ_64K | SZ_512M;

	if (arm_smmu_ops.pgsize_bitmap == -1UL)
		arm_smmu_ops.pgsize_bitmap = smmu->pgsize_bitmap;
	else
		arm_smmu_ops.pgsize_bitmap |= smmu->pgsize_bitmap;
	dev_notice(smmu->dev, "\tSupported page sizes: 0x%08lx\n",
		   smmu->pgsize_bitmap);


	if (smmu->features & ARM_SMMU_FEAT_TRANS_S1)
		dev_notice(smmu->dev, "\tStage-1: %lu-bit VA -> %lu-bit IPA\n",
			   smmu->va_size, smmu->ipa_size);

	if (smmu->features & ARM_SMMU_FEAT_TRANS_S2)
		dev_notice(smmu->dev, "\tStage-2: %lu-bit IPA -> %lu-bit PA\n",
			   smmu->ipa_size, smmu->pa_size);

	return 0;
}

struct arm_smmu_match_data {
	enum arm_smmu_arch_version version;
	enum arm_smmu_implementation model;
};

#define ARM_SMMU_MATCH_DATA(name, ver, imp)	\
static const struct arm_smmu_match_data name = { .version = ver, .model = imp }

ARM_SMMU_MATCH_DATA(smmu_generic_v1, ARM_SMMU_V1, GENERIC_SMMU);
ARM_SMMU_MATCH_DATA(smmu_generic_v2, ARM_SMMU_V2, GENERIC_SMMU);
ARM_SMMU_MATCH_DATA(arm_mmu401, ARM_SMMU_V1_64K, GENERIC_SMMU);
ARM_SMMU_MATCH_DATA(arm_mmu500, ARM_SMMU_V2, ARM_MMU500);
ARM_SMMU_MATCH_DATA(cavium_smmuv2, ARM_SMMU_V2, CAVIUM_SMMUV2);
ARM_SMMU_MATCH_DATA(qcom_smmuv2, ARM_SMMU_V2, QCOM_SMMUV2);

static const struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v1", .data = &smmu_generic_v1 },
	{ .compatible = "arm,smmu-v2", .data = &smmu_generic_v2 },
	{ .compatible = "arm,mmu-400", .data = &smmu_generic_v1 },
	{ .compatible = "arm,mmu-401", .data = &arm_mmu401 },
	{ .compatible = "arm,mmu-500", .data = &arm_mmu500 },
	{ .compatible = "cavium,smmu-v2", .data = &cavium_smmuv2 },
	{ .compatible = "nvidia,smmu-500", .data = &arm_mmu500 },
	{ .compatible = "qcom,qsmmu-v500", .data = &arm_mmu500 },
	{ .compatible = "qcom,smmu-v2", .data = &qcom_smmuv2 },
	{ },
};
MODULE_DEVICE_TABLE(of, arm_smmu_of_match);

#ifdef CONFIG_ACPI
static int acpi_smmu_get_data(u32 model, struct arm_smmu_device *smmu)
{
	int ret = 0;

	switch (model) {
	case ACPI_IORT_SMMU_V1:
	case ACPI_IORT_SMMU_CORELINK_MMU400:
		smmu->version = ARM_SMMU_V1;
		smmu->model = GENERIC_SMMU;
		break;
	case ACPI_IORT_SMMU_CORELINK_MMU401:
		smmu->version = ARM_SMMU_V1_64K;
		smmu->model = GENERIC_SMMU;
		break;
	case ACPI_IORT_SMMU_V2:
		smmu->version = ARM_SMMU_V2;
		smmu->model = GENERIC_SMMU;
		break;
	case ACPI_IORT_SMMU_CORELINK_MMU500:
		smmu->version = ARM_SMMU_V2;
		smmu->model = ARM_MMU500;
		break;
	case ACPI_IORT_SMMU_CAVIUM_THUNDERX:
		smmu->version = ARM_SMMU_V2;
		smmu->model = CAVIUM_SMMUV2;
		break;
	default:
		ret = -ENODEV;
	}

	return ret;
}

static int arm_smmu_device_acpi_probe(struct platform_device *pdev,
				      struct arm_smmu_device *smmu)
{
	struct device *dev = smmu->dev;
	struct acpi_iort_node *node =
		*(struct acpi_iort_node **)dev_get_platdata(dev);
	struct acpi_iort_smmu *iort_smmu;
	int ret;

	/* Retrieve SMMU1/2 specific data */
	iort_smmu = (struct acpi_iort_smmu *)node->node_data;

	ret = acpi_smmu_get_data(iort_smmu->model, smmu);
	if (ret < 0)
		return ret;

	/* Ignore the configuration access interrupt */
	smmu->num_global_irqs = 1;

	if (iort_smmu->flags & ACPI_IORT_SMMU_COHERENT_WALK)
		smmu->features |= ARM_SMMU_FEAT_COHERENT_WALK;

	return 0;
}
#else
static inline int arm_smmu_device_acpi_probe(struct platform_device *pdev,
					     struct arm_smmu_device *smmu)
{
	return -ENODEV;
}
#endif

static int arm_smmu_device_dt_probe(struct platform_device *pdev,
				    struct arm_smmu_device *smmu)
{
	const struct arm_smmu_match_data *data;
	struct device *dev = &pdev->dev;
	bool legacy_binding;

	if (of_property_read_u32(dev->of_node, "#global-interrupts",
				 &smmu->num_global_irqs)) {
		dev_err(dev, "missing #global-interrupts property\n");
		return -ENODEV;
	}

	data = of_device_get_match_data(dev);
	smmu->version = data->version;
	smmu->model = data->model;
	smmu->skip_init = of_property_read_bool(dev->of_node,
						 "qcom,skip-init");
	smmu->use_3lvl_tables = of_property_read_bool(dev->of_node,
							"qcom,use-3-lvl-tables");

	legacy_binding = of_find_property(dev->of_node, "mmu-masters", NULL);
	if (legacy_binding && !using_generic_binding) {
		if (!using_legacy_binding) {
			pr_notice("deprecated \"mmu-masters\" DT property in use; %s support unavailable\n",
				  IS_ENABLED(CONFIG_ARM_SMMU_LEGACY_DT_BINDINGS) ? "DMA API" : "SMMU");
		}
		using_legacy_binding = true;
	} else if (!legacy_binding && !using_legacy_binding) {
		using_generic_binding = true;
	} else {
		dev_err(dev, "not probing due to mismatched DT properties\n");
		return -ENODEV;
	}

	if (of_dma_is_coherent(dev->of_node))
		smmu->features |= ARM_SMMU_FEAT_COHERENT_WALK;

	return 0;
}

static int arm_smmu_bus_init(struct iommu_ops *ops)
{
	int err;

	/* Oh, for a proper bus abstraction */
	if (!iommu_present(&platform_bus_type)) {
		err = bus_set_iommu(&platform_bus_type, ops);
		if (err)
			return err;
	}
#ifdef CONFIG_ARM_AMBA
	if (!iommu_present(&amba_bustype)) {
		err = bus_set_iommu(&amba_bustype, ops);
		if (err)
			goto err_reset_platform_ops;
	}
#endif
#ifdef CONFIG_PCI
	if (!iommu_present(&pci_bus_type)) {
		err = bus_set_iommu(&pci_bus_type, ops);
		if (err)
			goto err_reset_amba_ops;
	}
#endif
#ifdef CONFIG_FSL_MC_BUS
	if (!iommu_present(&fsl_mc_bus_type)) {
		err = bus_set_iommu(&fsl_mc_bus_type, ops);
		if (err)
			goto err_reset_pci_ops;
	}
#endif
	return 0;

err_reset_pci_ops: __maybe_unused;
#ifdef CONFIG_PCI
	bus_set_iommu(&pci_bus_type, NULL);
#endif
err_reset_amba_ops: __maybe_unused;
#ifdef CONFIG_ARM_AMBA
	bus_set_iommu(&amba_bustype, NULL);
#endif
err_reset_platform_ops: __maybe_unused;
	bus_set_iommu(&platform_bus_type, NULL);
	return err;
}


/* A52_PHASE249_GPU_SMMU_ENODEV_ROOT_V1 */
static bool a52_k249_gpu_smmu(struct device *dev)
{
	const char *name = dev ? dev_name(dev) : NULL;

	return name && strstr(name, "3d40000") && dev->of_node &&
		of_device_is_compatible(dev->of_node, "qcom,smmu-v2");
}

static int arm_smmu_device_probe(struct platform_device *pdev)
{
	struct resource *res;
	resource_size_t ioaddr;
	struct arm_smmu_device *smmu;
	struct device *dev = &pdev->dev;
	bool trace = a52_apps_smmu(dev);
	int num_irqs, i, err;
	bool a52_pm_owned = false;
	bool a52_pm_ref = false;
	bool a52_sysfs_added = false;
	bool a52_iommu_registered = false;
	irqreturn_t (*global_fault)(int irq, void *dev);
	bool k249 = a52_k249_gpu_smmu(dev);

	if (trace)
		a52_ackfr_record("SMMU parent-probe enter dev=%s driver=%s",
			dev_name(dev), dev->driver ? dev->driver->name : "-");

	if (k249)
		a52_ackfr_record("K249 S ent");

	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);
	if (!smmu) {
		dev_err(dev, "failed to allocate arm_smmu_device\n");
		return -ENOMEM;
	}
	smmu->dev = dev;
	idr_init(&smmu->kgsl_asid_idr); /* A52_PHASE253_KGSL_SMMU_DOMAIN_CONTRACT_V1 */
	mutex_init(&smmu->kgsl_asid_lock);

	if (dev->of_node)
		err = arm_smmu_device_dt_probe(pdev, smmu);
	else
		err = arm_smmu_device_acpi_probe(pdev, smmu);

	if (k249)
		a52_ackfr_record("K249 S dt rc=%d", err);
	if (trace)
		a52_ackfr_record("SMMU parent-dt rc=%d model=%d skip=%d lvl3=%d",
			err, err ? -1 : smmu->model, smmu->skip_init,
			smmu->use_3lvl_tables);
	if (err)
		return err;

	if (k249)
		a52_ackfr_record("K249 S map in");
	smmu->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (k249)
		a52_ackfr_record("K249 S map rc=%d",
			IS_ERR(smmu->base) ? (int) PTR_ERR(smmu->base) : 0);
	if (IS_ERR(smmu->base))
		return PTR_ERR(smmu->base);
	ioaddr = res->start;
	/*
	 * The resource size should effectively match the value of SMMU_TOP;
	 * stash that temporarily until we know PAGESIZE to validate it with.
	 */
	smmu->numpage = resource_size(res);

	if (k249)
		a52_ackfr_record("K249 S impl in");
	smmu = arm_smmu_impl_init(smmu);
	if (k249)
		a52_ackfr_record("K249 S impl rc=%d",
			IS_ERR(smmu) ? (int) PTR_ERR(smmu) : 0);
	if (IS_ERR(smmu)) {
		err = PTR_ERR(smmu);
		if (trace)
			a52_ackfr_record("SMMU parent-impl rc=%d", err);
		return err;
	}
	if (trace)
		a52_ackfr_record("SMMU parent-impl rc=0 impl=%d", !!smmu->impl);

	num_irqs = 0;
	while ((res = platform_get_resource(pdev, IORESOURCE_IRQ, num_irqs))) {
		num_irqs++;
		if (num_irqs > smmu->num_global_irqs)
			smmu->num_context_irqs++;
	}

	if (k249)
		a52_ackfr_record("K249 S irqs n=%d g=%u c=%u", num_irqs,
			smmu->num_global_irqs, smmu->num_context_irqs);
	if (!smmu->num_context_irqs) {
		dev_err(dev, "found %d interrupts but expected at least %d\n",
			num_irqs, smmu->num_global_irqs + 1);
		return -ENODEV;
	}

	smmu->irqs = devm_kcalloc(dev, num_irqs, sizeof(*smmu->irqs),
				  GFP_KERNEL);
	if (!smmu->irqs) {
		dev_err(dev, "failed to allocate %d irqs\n", num_irqs);
		return -ENOMEM;
	}

	for (i = 0; i < num_irqs; ++i) {
		int irq = platform_get_irq(pdev, i);

		if (irq < 0)
			return -ENODEV;
		smmu->irqs[i] = irq;
	}

	err = devm_clk_bulk_get_all(dev, &smmu->clks);
	if (k249)
		a52_ackfr_record("K249 S clkget rc=%d", err);
	if (err < 0) {
		dev_err(dev, "failed to get clocks %d\n", err);
		return err;
	}
	smmu->num_clks = err;

	err = a52_arm_smmu_init_gdscs(smmu);
	if (k249)
		a52_ackfr_record("K250 S gdscget rc=%d n=%d", err,
			err ? 0 : smmu->a52_num_gdscs);
	if (err)
		return err;

	err = a52_arm_smmu_enable_gdscs(smmu);
	if (k249)
		a52_ackfr_record("K250 S regon rc=%d n=%d", err,
			smmu->a52_num_gdscs);
	if (err)
		return err;

	err = clk_bulk_prepare_enable(smmu->num_clks, smmu->clks);
	if (k249) {
		a52_ackfr_record("K249 S clkon rc=%d", err);
		a52_ackfr_record("K250 S clkon rc=%d", err);
	}
	if (err) {
		a52_arm_smmu_disable_gdscs(smmu);
		return err;
	}

	err = arm_smmu_device_cfg_probe(smmu);
	if (k249)
		a52_ackfr_record("K249 S cfg rc=%d", err);
	if (trace)
		a52_ackfr_record("SMMU parent-cfg rc=%d groups=%u cbs=%u", err,
			smmu->num_mapping_groups, smmu->num_context_banks);
	if (err) {
		if (a52_kgsl_smmu_has_phase250_vdd(smmu))
			goto a52_probe_error;
		return err;
	}

	a52_p426_cache_boot_routes(smmu);

	if (smmu->version == ARM_SMMU_V2) {
		if (smmu->num_context_banks > smmu->num_context_irqs) {
			dev_err(dev,
			      "found only %d context irq(s) but %d required\n",
			      smmu->num_context_irqs, smmu->num_context_banks);
			if (a52_kgsl_smmu_has_phase250_vdd(smmu)) {
				err = -ENODEV;
				goto a52_probe_error;
			}
			return -ENODEV;
		}

		/* Ignore superfluous interrupts */
		smmu->num_context_irqs = smmu->num_context_banks;
	}

	if (smmu->impl && smmu->impl->global_fault)
		global_fault = smmu->impl->global_fault;
	else
		global_fault = arm_smmu_global_fault;

	for (i = 0; i < smmu->num_global_irqs; ++i) {
		if (k249)
			a52_ackfr_record("K249 S irq in i=%d n=%d", i, smmu->irqs[i]);
		err = devm_request_irq(smmu->dev, smmu->irqs[i],
				       global_fault,
				       IRQF_SHARED,
				       "arm-smmu global fault",
				       smmu);
		if (k249)
			a52_ackfr_record("K249 S irq rc=%d i=%d", err, i);
		if (err) {
			dev_err(dev, "failed to request global IRQ %d (%u)\n",
				i, smmu->irqs[i]);
			if (a52_kgsl_smmu_has_phase250_vdd(smmu))
				goto a52_probe_error;
			return err;
		}
	}

	err = iommu_device_sysfs_add(&smmu->iommu, smmu->dev, NULL,
				     "smmu.%pa", &ioaddr);
	if (k249)
		a52_ackfr_record("K249 S sys rc=%d", err);
	if (err) {
		dev_err(dev, "Failed to register iommu in sysfs\n");
		if (a52_kgsl_smmu_has_phase250_vdd(smmu))
			goto a52_probe_error;
		return err;
	}

	a52_sysfs_added = true;
	iommu_device_set_ops(&smmu->iommu, &arm_smmu_ops);
	iommu_device_set_fwnode(&smmu->iommu, dev->fwnode);

	err = iommu_device_register(&smmu->iommu);
	if (k249)
		a52_ackfr_record("K249 S reg rc=%d", err);
	if (trace)
		a52_ackfr_record("SMMU parent-register rc=%d", err);
	if (err) {
		dev_err(dev, "Failed to register iommu\n");
		if (a52_kgsl_smmu_has_phase250_vdd(smmu))
			goto a52_probe_error;
		return err;
	}

	a52_iommu_registered = true;
	platform_set_drvdata(pdev, smmu);
	arm_smmu_device_reset(smmu);
	arm_smmu_test_smr_masks(smmu);

	/*
	 * We want to avoid touching dev->power.lock in fastpaths unless
	 * it's really going to do something useful - pm_runtime_enabled()
	 * can serve as an ideal proxy for that decision. So, conditionally
	 * enable pm_runtime.
	 */
	/* A52_KGSL_SMMU_RPM_V1: target solely the Phase250 vdd SMMU.
	 * Preserve the separate QSMMUv500 unmanaged-TBU guard unchanged.
	 */
	if ((!a52_kgsl_smmu_has_phase250_vdd(smmu) &&
	     dev->pm_domain && !a52_apps_smmu_has_unmanaged_tbus(dev)) ||
	    (a52_kgsl_smmu_has_phase250_vdd(smmu) && a52_kgsl_smmu_rpm)) {
		if (a52_kgsl_smmu_has_phase250_vdd(smmu)) {
			pm_runtime_set_autosuspend_delay(dev, 20);
			pm_runtime_use_autosuspend(dev);
			a52_kgsl_smmu_pm_diag(dev, "probe_before_pm");
		}
		err = pm_runtime_set_active(dev);
		if (err && a52_kgsl_smmu_has_phase250_vdd(smmu))
			goto a52_probe_error;
		if (a52_kgsl_smmu_has_phase250_vdd(smmu)) {
			pm_runtime_get_noresume(dev);
			a52_pm_ref = true;
		}
		pm_runtime_enable(dev);
		a52_pm_owned = a52_kgsl_smmu_has_phase250_vdd(smmu);
	} else if (a52_apps_smmu_has_unmanaged_tbus(dev)) {
		dev_warn(dev,
			 "runtime PM disabled until qsmmuv500 TBU support is ported\n");
	}

	/*
	 * For ACPI and generic DT bindings, an SMMU will be probed before
	 * any device which might need it, so we want the bus ops in place
	 * ready to handle default domain setup as soon as any SMMU exists.
	 */
	if (!using_legacy_binding) {
		if (k249)
			a52_ackfr_record("K249 S bus in");
		err = arm_smmu_bus_init(&arm_smmu_ops);
		if (k249)
			a52_ackfr_record("K249 S bus rc=%d", err);
		if (trace)
			a52_ackfr_record("SMMU parent-probe exit rc=%d", err);
		if (err && a52_kgsl_smmu_has_phase250_vdd(smmu))
			goto a52_probe_error;
		if (a52_pm_ref) {
			a52_kgsl_smmu_pm_diag(dev, "probe_before_autosuspend");
			pm_runtime_mark_last_busy(dev);
			pm_runtime_put_autosuspend(dev);
			a52_kgsl_smmu_pm_diag(dev, "probe_after_autosuspend");
		}
		return err;
	}

	if (trace)
		a52_ackfr_record("SMMU parent-probe exit rc=0");
	if (k249)
		a52_ackfr_record("K249 S exit rc=0");
	if (a52_pm_ref) {
		a52_kgsl_smmu_pm_diag(dev, "probe_before_autosuspend");
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
		a52_kgsl_smmu_pm_diag(dev, "probe_after_autosuspend");
	}
	return 0;

a52_probe_error:
	/* Only unwind the regulators acquired by this failed A52 probe.
	 * Do not change successful runtime behavior or other SMMU nodes.
	 */
	pr_err("P446S KGSL_SMMU_RPM probe_error dev=%s rc=%d pm_owned=%u\n",
	       dev_name(dev), err, a52_pm_owned);
	if (a52_pm_owned)
		pm_runtime_disable(dev);
	if (a52_pm_ref)
		pm_runtime_put_noidle(dev);
	pm_runtime_dont_use_autosuspend(dev);
	if (a52_iommu_registered)
		iommu_device_unregister(&smmu->iommu);
	if (a52_sysfs_added)
		iommu_device_sysfs_remove(&smmu->iommu);
	clk_bulk_disable(smmu->num_clks, smmu->clks);
	a52_arm_smmu_disable_gdscs(smmu);
	clk_bulk_unprepare(smmu->num_clks, smmu->clks);
	return err;
}

static int arm_smmu_device_remove(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);

	if (!smmu)
		return -ENODEV;

	idr_destroy(&smmu->kgsl_asid_idr); /* A52_PHASE253_KGSL_SMMU_DOMAIN_CONTRACT_V1 */

	if (!bitmap_empty(smmu->context_map, ARM_SMMU_MAX_CBS))
		dev_notice(&pdev->dev, "disabling translation\n");

	arm_smmu_bus_init(NULL);
	iommu_device_unregister(&smmu->iommu);
	iommu_device_sysfs_remove(&smmu->iommu);

	arm_smmu_rpm_get(smmu);
	/* Turn the thing off */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, ARM_SMMU_sCR0_CLIENTPD);
	arm_smmu_rpm_put(smmu);

	if (a52_kgsl_smmu_has_phase250_vdd(smmu))
		a52_kgsl_smmu_pm_diag(smmu->dev, "remove_begin");
	if (pm_runtime_enabled(smmu->dev))
		pm_runtime_force_suspend(smmu->dev);
	else {
		clk_bulk_disable(smmu->num_clks, smmu->clks);
		a52_arm_smmu_disable_gdscs(smmu);
	}

	if (a52_kgsl_smmu_has_phase250_vdd(smmu)) {
		pm_runtime_dont_use_autosuspend(smmu->dev);
		a52_kgsl_smmu_pm_diag(smmu->dev, "remove_done");
	}
	clk_bulk_unprepare(smmu->num_clks, smmu->clks);
	return 0;
}

static void arm_smmu_device_shutdown(struct platform_device *pdev)
{
	arm_smmu_device_remove(pdev);
}

static int __maybe_unused arm_smmu_runtime_resume(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	int ret;

	if (a52_kgsl_smmu_has_phase250_vdd(smmu))
		a52_kgsl_smmu_pm_diag(dev, "runtime_resume_begin");
	ret = a52_arm_smmu_enable_gdscs(smmu);
	if (ret)
		return ret;
	if (a52_kgsl_smmu_has_phase250_vdd(smmu))
		a52_kgsl_smmu_pm_diag(dev, "runtime_resume_vdd_on");

	ret = clk_bulk_enable(smmu->num_clks, smmu->clks);
	if (ret) {
		a52_arm_smmu_disable_gdscs(smmu);
		return ret;
	}

	arm_smmu_device_reset(smmu);

	return 0;
}

static int __maybe_unused arm_smmu_runtime_suspend(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);

	if (a52_apps_smmu_has_unmanaged_tbus(dev))
		return -EBUSY;

	if (a52_kgsl_smmu_has_phase250_vdd(smmu))
		a52_kgsl_smmu_pm_diag(dev, "runtime_suspend_begin");
	clk_bulk_disable(smmu->num_clks, smmu->clks);
	a52_arm_smmu_disable_gdscs(smmu);
	if (a52_kgsl_smmu_has_phase250_vdd(smmu))
		a52_kgsl_smmu_pm_diag(dev, "runtime_suspend_vdd_off");

	return 0;
}


/* A52_P460B_APPS_SMMU_TG_OPTIN_V1
 * EXPERIMENTAL. TG 4.19 skip-init resumes with global FSR/TLB and sCR0.
 * No SME or context bank writes. No power/domain/clock votes are altered.
 * This does NOT prove TCU/TBU state survives system sleep.
 */
static bool a52_p460b_apps_smmu_tg;
static bool a52_p460b_suspend_armed;
static u64 a52_p460b_pre_sme_sum;
static u32 a52_p460b_pre_scr0;
static u64 a52_p460b_cycle;
static struct kobject *a52_p460b_kobj;

static bool a52_p460b_scope(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);

	return smmu && smmu->skip_init &&
		of_machine_is_compatible("qcom,lagoon") &&
		!strcmp(dev_name(dev), "15000000.apps-smmu") &&
		a52_apps_smmu_has_unmanaged_tbus(dev) &&
		!pm_runtime_enabled(dev);
}

static u64 a52_p460b_sme_checksum(struct arm_smmu_device *smmu)
{
	u64 sum = 1469598103934665603ULL;
	unsigned int i;
	u32 value;

	/* MMIO read only. Do not mask or rewrite firmware-owned SMRs. */
	for (i = 0; i < smmu->num_mapping_groups; i++) {
		value = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
		sum = (sum ^ value) * 1099511628211ULL;
		value = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_S2CR(i));
		sum = (sum ^ value) * 1099511628211ULL;
	}
	return sum ^ smmu->num_mapping_groups;
}

static ssize_t apps_smmu_s2idle_tg_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 READ_ONCE(a52_p460b_apps_smmu_tg));
}

static ssize_t apps_smmu_s2idle_tg_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t size)
{
	bool on;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (kstrtobool(buf, &on))
		return -EINVAL;
	WRITE_ONCE(a52_p460b_apps_smmu_tg, on);
	pr_warn("P460B SMMU KNOB enable=%u (experimental, no TBU rail guarantee)\n",
		on);
	return size;
}

static struct kobj_attribute a52_p460b_tg_attr =
	__ATTR(apps_smmu_s2idle_tg, 0600,
	       apps_smmu_s2idle_tg_show, apps_smmu_s2idle_tg_store);

static int __init a52_p460b_sysfs_init(void)
{
	int ret;

	a52_p460b_kobj = kobject_create_and_add("a52_pm", kernel_kobj);
	if (!a52_p460b_kobj)
		return -ENOMEM;
	ret = sysfs_create_file(a52_p460b_kobj, &a52_p460b_tg_attr.attr);
	if (ret) {
		kobject_put(a52_p460b_kobj);
		a52_p460b_kobj = NULL;
	}
	return ret;
}
late_initcall(a52_p460b_sysfs_init);

static int a52_p460b_tg_suspend(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);

	a52_p460b_pre_scr0 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sCR0);
	a52_p460b_pre_sme_sum = a52_p460b_sme_checksum(smmu);
	a52_p460b_cycle++;
	WRITE_ONCE(a52_p460b_suspend_armed, true);
	dev_warn(dev,
		 "P460B SMMU SUSPEND cycle=%llu scr0=%08x sum=%016llx mappings=%u\n",
		 (unsigned long long)a52_p460b_cycle,
		 a52_p460b_pre_scr0,
		 (unsigned long long)a52_p460b_pre_sme_sum,
		 smmu->num_mapping_groups);
	/* TG has no suspend PM callback. No register/clock/power writes. */
	return 0;
}

static int a52_p460b_tg_resume(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	u32 gfsr, scr0_before, scr0_after;
	u64 after;
	bool match;

	scr0_before = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sCR0);
	gfsr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sGFSR);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sGFSR, gfsr);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIALLH, 0);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIALLNSNH, 0);
	arm_smmu_tlb_sync_global(smmu);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, a52_p460b_pre_scr0);

	scr0_after = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sCR0);
	after = a52_p460b_sme_checksum(smmu);
	match = (after == a52_p460b_pre_sme_sum &&
		 scr0_after == a52_p460b_pre_scr0);
	WRITE_ONCE(a52_p460b_suspend_armed, false);
	dev_warn(dev,
		 "P460B SMMU RESUME cycle=%llu scr0_before=%08x scr0_after=%08x expected=%08x sum_before=%016llx sum_after=%016llx match=%u\n",
		 (unsigned long long)a52_p460b_cycle,
		 scr0_before, scr0_after, a52_p460b_pre_scr0,
		 (unsigned long long)a52_p460b_pre_sme_sum,
		 (unsigned long long)after, match);
	if (!match) {
		WRITE_ONCE(a52_p460b_apps_smmu_tg, false);
		dev_crit(dev,
			 "P460B SMMU CORRUPTION: mismatch; tg knob forced 0. Mapping state is NOT repaired!\n");
		return -EIO;
	}
	return 0;
}

static int __maybe_unused arm_smmu_pm_resume(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);

	if (a52_p460b_scope(dev) &&
	    READ_ONCE(a52_p460b_suspend_armed))
		return a52_p460b_tg_resume(dev);
	if (smmu && a52_kgsl_smmu_has_phase250_vdd(smmu))
		a52_kgsl_smmu_pm_diag(dev, "system_resume");

	if (pm_runtime_suspended(dev))
		return 0;

	return arm_smmu_runtime_resume(dev);
}

static int __maybe_unused arm_smmu_pm_suspend(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	if (a52_apps_smmu_has_unmanaged_tbus(dev)) {
		if (READ_ONCE(a52_p460b_apps_smmu_tg) &&
		    a52_p460b_scope(dev))
			return a52_p460b_tg_suspend(dev);

		dev_warn(dev,
			 "system suspend blocked until qsmmuv500 TBU support is ported\n");
		return -EBUSY;
	}

	if (smmu && a52_kgsl_smmu_has_phase250_vdd(smmu))
		a52_kgsl_smmu_pm_diag(dev, "system_suspend");

	if (pm_runtime_suspended(dev))
		return 0;

	return arm_smmu_runtime_suspend(dev);
}

static const struct dev_pm_ops arm_smmu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(arm_smmu_pm_suspend, arm_smmu_pm_resume)
	SET_RUNTIME_PM_OPS(arm_smmu_runtime_suspend,
			   arm_smmu_runtime_resume, NULL)
};

static struct platform_driver arm_smmu_driver = {
	.driver	= {
		.name			= "arm-smmu",
		.of_match_table		= arm_smmu_of_match,
		.pm			= &arm_smmu_pm_ops,
		.suppress_bind_attrs    = true,
	},
	.probe	= arm_smmu_device_probe,
	.remove	= arm_smmu_device_remove,
	.shutdown = arm_smmu_device_shutdown,
};
module_platform_driver(arm_smmu_driver);

MODULE_DESCRIPTION("IOMMU API for ARM architected SMMU implementations");
MODULE_AUTHOR("Will Deacon <will@kernel.org>");
MODULE_ALIAS("platform:arm-smmu");
MODULE_LICENSE("GPL v2");

/* A52_PHASE411_FIFO_PANIC_SMMU_EXPERIMENT_V1: raw context-fault values are panic-visible. */
static const char a52_p411_smmu_marker[] __used = "A52_PHASE411_FIFO_PANIC_SMMU_EXPERIMENT_V1";

/* A52_PHASE427_CB2_HANDOFF_ROAD_PROBE_V1: exact SID800 firmware-CB reuse + postmortem state. */

/* A52_PHASE430_SF_DMA_FORENSICS_V1: passive state plus explicit ATOS. */
