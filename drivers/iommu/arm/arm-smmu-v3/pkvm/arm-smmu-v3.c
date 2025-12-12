// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>

#include "arm_smmu_v3.h"

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

/* Transfer ownership of memory */
static int smmu_take_pages(u64 phys, size_t size)
{
	WARN_ON(!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size));
	return __pkvm_host_donate_hyp(phys >> PAGE_SHIFT, size >> PAGE_SHIFT);
}

static void smmu_reclaim_pages(u64 phys, size_t size)
{
	WARN_ON(!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size));
	WARN_ON(__pkvm_hyp_donate_host(phys >> PAGE_SHIFT, size >> PAGE_SHIFT));
}

/* Put the device in a state that can be probed by the host driver. */
static void smmu_deinit_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int i;
	size_t nr_pages = smmu->mmio_size >> PAGE_SHIFT;

	for (i = 0 ; i < nr_pages ; ++i) {
		u64 pfn = (smmu->mmio_addr >> PAGE_SHIFT) + i;

		WARN_ON(__pkvm_hyp_donate_host(pfn, 1));
	}
}

static int smmu_init_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int i;
	size_t nr_pages;

	if (!PAGE_ALIGNED(smmu->mmio_addr | smmu->mmio_size))
		return -EINVAL;

	nr_pages = smmu->mmio_size >> PAGE_SHIFT;
	for (i = 0 ; i < nr_pages ; ++i) {
		u64 pfn = (smmu->mmio_addr >> PAGE_SHIFT) + i;

		/*
		 * This should never happen, so it's fine to be strict to avoid
		 * complicated error handling.
		 */
		WARN_ON(___pkvm_host_donate_hyp(pfn, 1, true));
	}
	smmu->base = hyp_phys_to_virt(smmu->mmio_addr);

	return 0;
}

static int smmu_init(void)
{
	int ret;
	struct hyp_arm_smmu_v3_device *smmu;
	size_t smmu_arr_size = PAGE_ALIGN(sizeof(*kvm_hyp_arm_smmu_v3_smmus) *
					  kvm_hyp_arm_smmu_v3_count);

	kvm_hyp_arm_smmu_v3_smmus = kern_hyp_va(kvm_hyp_arm_smmu_v3_smmus);
	ret = smmu_take_pages(hyp_virt_to_phys(kvm_hyp_arm_smmu_v3_smmus),
			      smmu_arr_size);
	if (ret)
		return ret;

	for_each_smmu(smmu) {
		ret = smmu_init_device(smmu);
		if (ret)
			goto out_reclaim_smmu;
	}

	return 0;

out_reclaim_smmu:
	while (smmu != kvm_hyp_arm_smmu_v3_smmus)
		smmu_deinit_device(--smmu);
	smmu_reclaim_pages(hyp_virt_to_phys(kvm_hyp_arm_smmu_v3_smmus),
			   smmu_arr_size);
	return ret;
}

static void smmu_host_stage2_idmap(phys_addr_t start, phys_addr_t end, int prot)
{
}

/* Shared with the kernel driver in EL1 */
struct kvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
	.host_stage2_idmap		= smmu_host_stage2_idmap,
};
