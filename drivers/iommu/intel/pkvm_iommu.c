// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2026 Google.
 */

#define pr_fmt(fmt)     "DMAR: pkvm: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include "iommu.h"

int __init pkvm_host_prepare_iommu(void)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;
	int ret;

	down_write(&dmar_global_lock);
	ret = dmar_table_init();
	if (ret) {
		pr_err("Failed to initialize DMAR table!\n");
		goto out;
	}

	ret = -ENODEV;
	for_each_iommu(iommu, drhd) {
		unsigned int pgsz_mask = 1 << PG_LEVEL_4K;
		unsigned int pglvl_mask = 0;

		if (drhd->ignored) {
			pr_warn("iommu%d ignored, but pKVM needs iommu to be enabled!\n",
				iommu->seq_id);
			goto out;
		}

		if (readl(iommu->reg + DMAR_GSTS_REG) & DMA_GSTS_TES) {
			pr_warn("iommu%d: Translation enabled before initialization!\n",
					iommu->seq_id);
			goto out;
		}

		/*
		 * Since pKVM is not expected to be supported on ancient hardware which
		 * requires write buffer flushing, require cap_rwbf=0 for simplicity.
		 */
		if (cap_rwbf(iommu->cap)) {
			pr_warn("iommu%d: CAP.RWBF=1 is not supported!\n", iommu->seq_id);
			goto out;
		}

		/* pKVM expects Queued Invalidation support for simplicity and efficiency */
		if (!ecap_qis(iommu->ecap)) {
			pr_warn("iommu%d: queued Invalidation not supported!\n", iommu->seq_id);
			goto out;
		}

		if (cap_sagaw(iommu->cap) & IOMMU_PGT_4LEVEL)
			pglvl_mask |= IOMMU_PGT_4LEVEL;
		if (cap_sagaw(iommu->cap) & IOMMU_PGT_5LEVEL)
			pglvl_mask |= IOMMU_PGT_5LEVEL;

		if (cap_super_page_val(iommu->cap) & BIT(0))
			pgsz_mask |= 1 << PG_LEVEL_2M;
		if (cap_super_page_val(iommu->cap) & BIT(1))
			pgsz_mask |= 1 << PG_LEVEL_1G;

		if (!pglvl_mask) {
			pr_warn("iommu%d: No supported page levels\n", iommu->seq_id);
			goto out;
		}

		pkvm_sym(iommu_pglvl_mask) &= pglvl_mask;
		pkvm_sym(iommu_pgsz_mask) &= pgsz_mask;
	}

	pkvm_sym(intel_iommu_sm) = intel_iommu_sm;

	for_each_iommu(iommu, drhd) {
		struct intel_iommu_info info = {
			.reg_phys = iommu->reg_phys,
			.reg_size = iommu->reg_size,
			.cap = iommu->cap,
			.ecap = iommu->ecap,
			.seq_id = iommu->seq_id,
			.agaw = iommu->agaw,
			.msagaw = iommu->msagaw,
		};

		ret = pkvm_sym(prepare_iommu)(&info);
		if (ret)
			goto out;
	}
out:
	up_write(&dmar_global_lock);
	return ret;
}

int __init pkvm_host_init_iommu(void)
{
	int ret = intel_iommu_init();

	if (!ret)
		pr_info("IOMMU initialized!\n");
	else
		pr_err("IOMMU initialize failed(err=%d)\n", ret);

	return ret;
}

int pkvm_qi_submit_sync(struct intel_iommu *iommu, struct qi_desc *desc,
			unsigned int count, unsigned long options)
{
	struct qi_desc *desc_ptr;
	int ret;

	desc_ptr = kcalloc(count, sizeof(struct qi_desc), GFP_ATOMIC);
	if (!desc_ptr)
		return -ENOMEM;

	memcpy(desc_ptr, desc, count * sizeof(struct qi_desc));
	ret = pkvm_hypercall(iommu_qi_submit, iommu->reg_phys,
			     virt_to_phys(desc_ptr), count, options);
	kfree(desc_ptr);
	return ret;
}
