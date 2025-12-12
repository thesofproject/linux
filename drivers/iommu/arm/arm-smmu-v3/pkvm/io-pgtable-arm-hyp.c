// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Arm Ltd.
 */
#include <nvhe/iommu.h>

#include <linux/io-pgtable.h>
#include "../../../io-pgtable-arm.h"

struct io_pgtable_ops *kvm_alloc_io_pgtable_ops(enum io_pgtable_fmt fmt,
						struct io_pgtable_cfg *cfg,
						void *cookie)
{
	struct io_pgtable *iop;

	if (fmt != ARM_64_LPAE_S2)
		return NULL;

	iop = arm_64_lpae_alloc_pgtable_s2(cfg, cookie);
	if (!iop)
		return NULL;

	iop->fmt	= fmt;
	iop->cookie	= cookie;
	iop->cfg	= *cfg;

	return &iop->ops;
}

void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
			     struct io_pgtable_cfg *cfg, void *cookie)
{
	void *addr;

	addr = kvm_iommu_donate_pages(get_order(size));

	if (addr && !cfg->coherent_walk)
		kvm_flush_dcache_to_poc(addr, size);

	return addr;
}

void __arm_lpae_free_pages(void *addr, size_t size, struct io_pgtable_cfg *cfg,
			   void *cookie)
{
	if (!cfg->coherent_walk)
		kvm_flush_dcache_to_poc(addr, size);

	kvm_iommu_reclaim_pages(addr);
}

void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
			 struct io_pgtable_cfg *cfg)
{
	if (!cfg->coherent_walk)
		kvm_flush_dcache_to_poc(ptep, sizeof(*ptep) * num_entries);
}

/* At the moment this is only used once, so rounding up to a page is not really a problem. */
void *__arm_lpae_alloc_data(size_t size, gfp_t gfp)
{
	return kvm_iommu_donate_pages(get_order(size));
}

void __arm_lpae_free_data(void *p)
{
	return kvm_iommu_reclaim_pages(p);
}
