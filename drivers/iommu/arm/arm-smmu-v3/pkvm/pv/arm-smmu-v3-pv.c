// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <asm/kvm_hyp.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>

#include "arm_smmu_v3.h"
#include "arm-smmu-v3-lib-hyp.h"
#include "arm-smmu-v3-module.h"

#ifdef MODULE
void *memset(void *dst, int c, size_t count)
{
	return CALL_FROM_OPS(memset, dst, c, count);
}

void *__memset(void *dst, int c, size_t count)
{
	return memset(dst, c, count);
}
#ifdef CONFIG_LIST_HARDENED
bool __list_add_valid_or_report(struct list_head *new,
				struct list_head *prev,
				struct list_head *next)
{
	return CALL_FROM_OPS(list_add_valid_or_report, new, prev, next);
}

bool __list_del_entry_valid_or_report(struct list_head *entry)
{
	return CALL_FROM_OPS(list_del_entry_valid_or_report, entry);
}
#endif

const struct pkvm_module_ops		*mod_ops;
#endif

size_t __ro_after_init kvm_hyp_arm_smmu_v3_pv_count;
struct hyp_arm_smmu_v3_device_pv *kvm_hyp_arm_smmu_v3_pv_smmus;

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_pv_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_pv_smmus[kvm_hyp_arm_smmu_v3_pv_count]; \
	     (smmu)++)

static int smmu_write_cr0(struct hyp_arm_smmu_v3_device *smmu, u32 val)
{
	writel_relaxed(val, smmu->base + ARM_SMMU_CR0);
	return smmu_wait(false, readl_relaxed(smmu->base + ARM_SMMU_CR0ACK) == val);
}

__maybe_unused
static int smmu_send_cmd(struct hyp_arm_smmu_v3_device_pv *smmu,
			 struct arm_smmu_cmdq_ent *cmd)
{
	int ret = smmu_add_cmd(&smmu->common, cmd);

	if (ret)
		return ret;

	return smmu_sync_cmd(&smmu->common);
}

static int smmu_init_registers(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 val, old;
	int ret;

	if (!(readl_relaxed(smmu->base + ARM_SMMU_GBPA) & GBPA_ABORT))
		return -EINVAL;

	/* Initialize all RW registers that will be read by the SMMU */
	ret = smmu_write_cr0(smmu, 0);
	if (ret)
		return ret;

	val = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);
	writel_relaxed(val, smmu->base + ARM_SMMU_CR1);
	writel_relaxed(CR2_PTM, smmu->base + ARM_SMMU_CR2);

	val = readl_relaxed(smmu->base + ARM_SMMU_GERROR);
	old = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);
	/* Service Failure Mode is fatal */
	if ((val ^ old) & GERROR_SFM_ERR)
		return -EIO;
	/* Clear pending errors */
	writel_relaxed(val, smmu->base + ARM_SMMU_GERRORN);

	return 0;
}

static int smmu_init_cmdq(struct hyp_arm_smmu_v3_device *smmu)
{
	size_t cmdq_size;
	int ret;
	enum kvm_pgtable_prot prot = PAGE_HYP;

	cmdq_size = (1 << (smmu->cmdq.llq.max_n_shift)) *
		     CMDQ_ENT_DWORDS * 8;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		prot |= KVM_PGTABLE_PROT_NORMAL_NC;

	ret = ___pkvm_host_donate_hyp(smmu->cmdq.base_dma >> PAGE_SHIFT,
				      PAGE_ALIGN(cmdq_size) >> PAGE_SHIFT, prot);
	if (ret)
		return ret;

	smmu->cmdq.base = hyp_phys_to_virt(smmu->cmdq.base_dma);
	smmu->cmdq.prod_reg = smmu->base + ARM_SMMU_CMDQ_PROD;
	smmu->cmdq.cons_reg = smmu->base + ARM_SMMU_CMDQ_CONS;
	memset(smmu->cmdq.base, 0, cmdq_size);
	writel_relaxed(0, smmu->cmdq.prod_reg);
	writel_relaxed(0, smmu->cmdq.cons_reg);

	return 0;
}

static int smmu_init_device(struct hyp_arm_smmu_v3_device_pv *smmu)
{
	int i, ret;
	size_t nr_pages;

	if (!PAGE_ALIGNED(smmu->common.mmio_addr | smmu->common.mmio_size))
		return -EINVAL;

	nr_pages = smmu->common.mmio_size >> PAGE_SHIFT;
	for (i = 0 ; i < nr_pages ; ++i) {
		u64 pfn = (smmu->common.mmio_addr >> PAGE_SHIFT) + i;

		/*
		 * This should never happen, so it's fine to be strict to avoid
		 * complicated error handling.
		 */
		WARN_ON(___pkvm_host_donate_hyp(pfn, 1, true));
	}
	smmu->common.base = hyp_phys_to_virt(smmu->common.mmio_addr);

	ret = smmu_init_registers(&smmu->common);
	if (ret)
		return ret;

	return smmu_init_cmdq(&smmu->common);
}

static int smmu_init(void)
{
	int ret;
	struct hyp_arm_smmu_v3_device_pv *smmu;
	size_t smmu_arr_size = PAGE_ALIGN(sizeof(*kvm_hyp_arm_smmu_v3_pv_smmus) *
					  kvm_hyp_arm_smmu_v3_pv_count);

	kvm_hyp_arm_smmu_v3_pv_smmus = kern_hyp_va(kvm_hyp_arm_smmu_v3_pv_smmus);
	ret = smmu_take_pages(hyp_virt_to_phys(kvm_hyp_arm_smmu_v3_pv_smmus),
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
	smmu_reclaim_pages(hyp_virt_to_phys(kvm_hyp_arm_smmu_v3_pv_smmus),
			   smmu_arr_size);
	return ret;
}

#ifdef MODULE
int smmu_init_hyp_module(const struct pkvm_module_ops *ops)
{
	if (!ops)
		return -EINVAL;

	mod_ops = ops;
	return 0;
}
#endif

/* Shared with the kernel driver in EL1 */
struct kvm_iommu_ops smmu_pv_ops = {
	.init                           = smmu_init,
};
