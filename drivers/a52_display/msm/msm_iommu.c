/*
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

#include "msm_drv.h"
#include "msm_mmu.h"
#include <linux/a52_ack_secure_flight_recorder.h>

static atomic_t a52_r179_iommu_map_count = ATOMIC_INIT(0);
static atomic_t a52_r179_iommu_unmap_count = ATOMIC_INIT(0);

struct msm_iommu {
	struct msm_mmu base;
	struct iommu_domain *domain;
};
#define to_msm_iommu(x) container_of(x, struct msm_iommu, base)

static int msm_fault_handler(struct iommu_domain *domain, struct device *dev,
		unsigned long iova, int flags, void *arg)
{
	struct msm_iommu *iommu = arg;
	if (iommu->base.handler)
		return iommu->base.handler(iommu->base.arg, iova, flags);
	pr_warn_ratelimited("*** fault: iova=%08lx, flags=%d\n", iova, flags);
	return 0;
}

static int msm_iommu_attach(struct msm_mmu *mmu, const char * const *names,
			    int cnt)
{
	struct msm_iommu *iommu = to_msm_iommu(mmu);
	int ret;

	pm_runtime_get_sync(mmu->dev);
	ret = iommu_attach_device(iommu->domain, mmu->dev);
	pm_runtime_put_sync(mmu->dev);

	return ret;
}

static void msm_iommu_detach(struct msm_mmu *mmu, const char * const *names,
			     int cnt)
{
	struct msm_iommu *iommu = to_msm_iommu(mmu);

	pm_runtime_get_sync(mmu->dev);
	iommu_detach_device(iommu->domain, mmu->dev);
	pm_runtime_put_sync(mmu->dev);
}

static int msm_iommu_map(struct msm_mmu *mmu, uint64_t iova,
		struct sg_table *sgt, unsigned len, int prot)
{
	struct msm_iommu *iommu = to_msm_iommu(mmu);
	size_t ret;

//	pm_runtime_get_sync(mmu->dev);
	ret = iommu_map_sg(iommu->domain, iova, sgt->sgl, sgt->nents, prot);
//	pm_runtime_put_sync(mmu->dev);
	WARN_ON(!ret);
	if (atomic_inc_return(&a52_r179_iommu_map_count) <= 64 || ret != len)
		a52_ackfr_record("DISP IOMMU map iova=%llx len=%u n=%u prot=%x mapped=%zu",
			(unsigned long long)iova, len, sgt->nents, prot, ret);

	return (ret == len) ? 0 : -EINVAL;
}

static int msm_iommu_unmap(struct msm_mmu *mmu, uint64_t iova,
		struct sg_table *sgt, unsigned len)
{
	struct msm_iommu *iommu = to_msm_iommu(mmu);

	if (atomic_inc_return(&a52_r179_iommu_unmap_count) <= 32)
		a52_ackfr_record("DISP IOMMU unmap iova=%llx len=%u",
				(unsigned long long)iova, len);
	pm_runtime_get_sync(mmu->dev);
	iommu_unmap(iommu->domain, iova, len);
	pm_runtime_put_sync(mmu->dev);

	return 0;
}

static void msm_iommu_destroy(struct msm_mmu *mmu)
{
	struct msm_iommu *iommu = to_msm_iommu(mmu);
	iommu_domain_free(iommu->domain);
	kfree(iommu);
}

static const struct msm_mmu_funcs funcs = {
		.attach = msm_iommu_attach,
		.detach = msm_iommu_detach,
		.map = msm_iommu_map,
		.unmap = msm_iommu_unmap,
		.destroy = msm_iommu_destroy,
};

struct msm_mmu *msm_iommu_new(struct device *dev, struct iommu_domain *domain)
{
	struct msm_iommu *iommu;

	iommu = kzalloc(sizeof(*iommu), GFP_KERNEL);
	if (!iommu)
		return ERR_PTR(-ENOMEM);

	iommu->domain = domain;
	msm_mmu_init(&iommu->base, dev, &funcs);
	iommu_set_fault_handler(domain, msm_fault_handler, iommu);

	return &iommu->base;
}
