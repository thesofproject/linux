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

static int smmu_send_cmd(struct hyp_arm_smmu_v3_device_pv *smmu,
			 struct arm_smmu_cmdq_ent *cmd)
{
	int ret = smmu_add_cmd(&smmu->common, cmd);

	if (ret)
		return ret;

	return smmu_sync_cmd(&smmu->common);
}

__maybe_unused
static int smmu_sync_ste(struct hyp_arm_smmu_v3_device_pv *smmu, __le64 *step, u32 sid)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CFGI_STE,
		.cfgi.sid = sid,
		.cfgi.leaf = true,
	};

	if (!(smmu->common.features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(step, STRTAB_STE_DWORDS << 3);

	return smmu_send_cmd(smmu, &cmd);
}

static int smmu_alloc_l2_strtab(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	struct arm_smmu_strtab_l1 *l1_desc;
	struct arm_smmu_strtab_l2 *l2table;
	u32 l1_idx = arm_smmu_strtab_l1_idx(sid);

	if (l1_idx >= cfg->l2.num_l1_ents)
		return -EINVAL;

	l1_desc = &cfg->l2.l1tab[l1_idx];
	if (l1_desc->l2ptr)
		return 0;

	l2table = kvm_iommu_donate_pages(get_order(sizeof(*l2table)), 0);
	if (!l2table)
		return -ENOMEM;

	/* Ensure the empty stream table is visible before the descriptor write */
	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(l2table, sizeof(*l2table));
	wmb();
	arm_smmu_write_strtab_l1_desc(l1_desc, hyp_virt_to_phys(l2table));
	return 0;
}

static struct arm_smmu_ste *
smmu_get_ste_ptr(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		u32 l1_idx = arm_smmu_strtab_l1_idx(sid);
		struct arm_smmu_strtab_l2 *l2ptr;

		if (l1_idx >= cfg->l2.num_l1_ents)
			return NULL;
		l2ptr = hyp_phys_to_virt(cfg->l2.l1tab[l1_idx].l2ptr & STRTAB_L1_DESC_L2PTR_MASK);
		/* Two-level walk */
		return &l2ptr->stes[arm_smmu_strtab_l2_idx(sid)];
	}

	if (sid >= cfg->linear.num_ents)
		return NULL;
	/* Simple linear lookup */
	return &cfg->linear.table[sid];
}

__maybe_unused
static struct arm_smmu_ste *
smmu_get_alloc_ste_ptr(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		int ret = smmu_alloc_l2_strtab(smmu, sid);

		if (ret)
			return NULL;
	}
	return smmu_get_ste_ptr(smmu, sid);
}

static int smmu_init_strtab(struct hyp_arm_smmu_v3_device *smmu)
{
	size_t strtab_size;
	u64 strtab_base;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	enum kvm_pgtable_prot prot = PAGE_HYP;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		prot |= KVM_PGTABLE_PROT_NORMAL_NC;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		strtab_size = PAGE_ALIGN(cfg->l2.num_l1_ents * sizeof(struct arm_smmu_strtab_l1));
		strtab_base = (u64)cfg->l2.l1_dma;
		cfg->linear.table = hyp_phys_to_virt(strtab_base);
	} else {
		strtab_size = PAGE_ALIGN(cfg->linear.num_ents * sizeof(struct arm_smmu_ste));
		strtab_base = (u64)cfg->linear.ste_dma;
		cfg->l2.l1tab = hyp_phys_to_virt(strtab_base);
	}
	return ___pkvm_host_donate_hyp(hyp_phys_to_pfn(strtab_base),
				       strtab_size >> PAGE_SHIFT, prot);
}

static int smmu_init_evtq(struct hyp_arm_smmu_v3_device_pv *smmu)
{
	size_t evtq_size, evtq_nr_pages;
	size_t i;
	int ret;

	evtq_size = (1 << (smmu->evtq.llq.max_n_shift)) *
		     EVTQ_ENT_DWORDS * 8;
	evtq_nr_pages = PAGE_ALIGN(evtq_size) >> PAGE_SHIFT;
	for (i = 0 ; i < evtq_nr_pages ; ++i) {
		u64 evtq_pfn = hyp_phys_to_pfn(smmu->evtq.base_dma) + i;
		/*
		 * Evtq is not accessed by hyp, but set in shared state
		 * to prevent donation/sharing it to VMs.
		 */
		ret = __pkvm_host_share_hyp(evtq_pfn);
		if (ret)
			return ret;
	}
	return hyp_pin_shared_mem(hyp_phys_to_virt(smmu->evtq.base_dma),
				  hyp_phys_to_virt(smmu->evtq.base_dma + evtq_size));
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
	ret = smmu_init_cmdq(&smmu->common);
	if (ret)
		return ret;

	ret = smmu_init_evtq(smmu);
	if (ret)
		return ret;

	return smmu_init_strtab(&smmu->common);
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
