// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/trap_handler.h>

#include "arm_smmu_v3.h"

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

#define cmdq_size(cmdq)	((1 << ((cmdq)->llq.max_n_shift)) * CMDQ_ENT_DWORDS * 8)

static bool is_cmdq_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_CMDQEN, smmu->cr0);
}

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

/*
 * CMDQ, STE host copies are accessed by the hypervisor, we share them to
 * - Prevent the host from passing protected VM memory.
 * - Having them mapped in the hyp page table.
 */
static int smmu_share_pages(phys_addr_t addr, size_t size)
{
	int i;
	size_t nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	for (i = 0 ; i < nr_pages ; ++i)
		WARN_ON(__pkvm_host_share_hyp((addr + i * PAGE_SIZE) >> PAGE_SHIFT));

	return hyp_pin_shared_mem(hyp_phys_to_virt(addr), hyp_phys_to_virt(addr + size));
}

static int smmu_unshare_pages(phys_addr_t addr, size_t size)
{
	int i;
	size_t nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	hyp_unpin_shared_mem(hyp_phys_to_virt(addr), hyp_phys_to_virt(addr + size));

	for (i = 0 ; i < nr_pages ; ++i)
		WARN_ON(__pkvm_host_unshare_hyp((addr + i * PAGE_SIZE) >> PAGE_SHIFT));

	return 0;
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

/*
 * Mini-probe and validation for the hypervisor.
 */
static int smmu_probe(struct hyp_arm_smmu_v3_device *smmu)
{
	u32 reg;

	/* IDR0 */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR0);
	smmu->features = smmu_idr0_features(reg);

	/*
	 * Some MMU600 and MMU700 have errata that prevent them from using nesting,
	 * not sure how can we identify those, so it's recommended not to enable this
	 * drivers on such systems.
	 * And preventing any of those will be too restrictive
	 */
	if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S1) ||
	    !(smmu->features & ARM_SMMU_FEAT_TRANS_S2))
		return -ENXIO;

	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR1);
	if (reg & (IDR1_TABLES_PRESET | IDR1_QUEUES_PRESET | IDR1_REL | IDR1_ECMDQ))
		return -EINVAL;

	smmu->sid_bits = FIELD_GET(IDR1_SIDSIZE, reg);
	/* Follows the kernel logic */
	if (smmu->sid_bits <= STRTAB_SPLIT)
		smmu->features &= ~ARM_SMMU_FEAT_2_LVL_STRTAB;

	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR3);
	smmu->features |= smmu_idr3_features(reg);

	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR5);
	smmu->pgsize_bitmap = smmu_idr5_to_pgsize(reg);

	smmu->oas = smmu_idr5_to_oas(reg);
	if (smmu->oas == 52)
		smmu->pgsize_bitmap |= 1ULL << 42;
	else if (!smmu->oas)
		smmu->oas = 48;

	smmu->ias = 64;
	smmu->ias = min(smmu->ias, smmu->oas);
	return 0;
}

/*
 * The kernel part of the driver will allocate the shadow cmdq,
 * and zero it. This function only donates it.
 */
static int smmu_init_cmdq(struct hyp_arm_smmu_v3_device *smmu)
{
	size_t cmdq_nr_pages = cmdq_size(&smmu->cmdq) >> PAGE_SHIFT;
	int ret;
	enum kvm_pgtable_prot prot = PAGE_HYP;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		prot |= KVM_PGTABLE_PROT_NORMAL_NC;

	ret = ___pkvm_host_donate_hyp_prot(smmu->cmdq.base_dma >> PAGE_SHIFT,
					   cmdq_nr_pages, false, prot);
	if (ret)
		return ret;

	smmu->cmdq.base = hyp_phys_to_virt(smmu->cmdq.base_dma);
	smmu->cmdq.prod_reg = smmu->base + ARM_SMMU_CMDQ_PROD;
	smmu->cmdq.cons_reg = smmu->base + ARM_SMMU_CMDQ_CONS;
	smmu->cmdq.q_base = smmu->cmdq.base_dma |
			    FIELD_PREP(Q_BASE_LOG2SIZE, smmu->cmdq.llq.max_n_shift);
	smmu->cmdq.ent_dwords = CMDQ_ENT_DWORDS;
	writel_relaxed(0, smmu->cmdq.prod_reg);
	writel_relaxed(0, smmu->cmdq.cons_reg);
	writeq_relaxed(smmu->cmdq.q_base, smmu->base + ARM_SMMU_CMDQ_BASE);
	return 0;
}

static int smmu_init_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int i, ret;
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
	ret = smmu_probe(smmu);
	if (ret)
		goto out_ret;
	hyp_spin_lock_init(&smmu->lock);

	ret = smmu_init_cmdq(smmu);
	if (ret)
		goto out_ret;

	return 0;

out_ret:
	smmu_deinit_device(smmu);
	return ret;
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

	BUILD_BUG_ON(sizeof(hyp_spinlock_t) != sizeof(u32));

	return 0;

out_reclaim_smmu:
	while (smmu != kvm_hyp_arm_smmu_v3_smmus)
		smmu_deinit_device(--smmu);
	smmu_reclaim_pages(hyp_virt_to_phys(kvm_hyp_arm_smmu_v3_smmus),
			   smmu_arr_size);
	return ret;
}

static void smmu_emulate_cmdq_enable(struct hyp_arm_smmu_v3_device *smmu)
{
	smmu->cmdq_host.llq.max_n_shift = smmu->cmdq_host.q_base & Q_BASE_LOG2SIZE;
	smmu->cmdq_host.base_dma = smmu->cmdq_host.q_base & Q_BASE_ADDR_MASK;
	WARN_ON(smmu_share_pages(smmu->cmdq_host.base_dma,
				 cmdq_size(&smmu->cmdq_host)));
}

static void smmu_emulate_cmdq_disable(struct hyp_arm_smmu_v3_device *smmu)
{
	WARN_ON(smmu_unshare_pages(smmu->cmdq_host.base_dma,
				   cmdq_size(&smmu->cmdq_host)));
}

static bool smmu_dabt_device(struct hyp_arm_smmu_v3_device *smmu,
			     struct user_pt_regs *regs,
			     u64 esr, u32 off)
{
	bool is_write = esr & ESR_ELx_WNR;
	unsigned int len = BIT((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT);
	int rd = (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT;
	const u64 read_write = -1ULL;
	const u64 no_access = 0;
	u64 mask = no_access;
	const u64 read_only = is_write ? no_access : read_write;
	u64 val = regs->regs[rd];

	switch (off) {
	case ARM_SMMU_IDR0:
		/* Clear stage-2 support, hide MSI to avoid write back to cmdq */
		mask = read_only & ~(IDR0_S2P | IDR0_VMID16 | IDR0_MSI | IDR0_HYP);
		WARN_ON(len != sizeof(u32));
		break;
	/* Passthrough the register access for bisectiblity, handled later */
	case ARM_SMMU_CMDQ_BASE:
		if (is_write) {
			/* Not allowed by the architecture */
			WARN_ON(is_cmdq_enabled(smmu));
			smmu->cmdq_host.q_base = val;
		}
		mask = read_write;
		break;
	case ARM_SMMU_CMDQ_PROD:
	case ARM_SMMU_CMDQ_CONS:
	case ARM_SMMU_STRTAB_BASE:
	case ARM_SMMU_STRTAB_BASE_CFG:
	case ARM_SMMU_GBPA:
		mask = read_write;
		break;
	case ARM_SMMU_CR0:
		if (is_write) {
			bool last_cmdq_en = is_cmdq_enabled(smmu);

			smmu->cr0 = val;
			if (!last_cmdq_en && is_cmdq_enabled(smmu))
				smmu_emulate_cmdq_enable(smmu);
			else if (last_cmdq_en && !is_cmdq_enabled(smmu))
				smmu_emulate_cmdq_disable(smmu);
		}
		mask = read_write;
		WARN_ON(len != sizeof(u32));
		break;
	case ARM_SMMU_CR1: {
		/* Based on Linux implementation */
		u64 cr2_template = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
				FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
				FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
				FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
				FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
				FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);
		/* Don't mess with shareability/cacheability. */
		if (is_write)
			WARN_ON(val != cr2_template);
		mask = read_write;
		WARN_ON(len != sizeof(u32));
		break;
	}
	/*
	 * These should be safe, just enforce RO or RW and size according to architecture.
	 * There are some other registers that are not used by Linux as IDR2, IDR4
	 * that won't be allowed.
	 */
	case ARM_SMMU_EVTQ_PROD + SZ_64K:
	case ARM_SMMU_EVTQ_CONS + SZ_64K:
	case ARM_SMMU_EVTQ_IRQ_CFG1:
	case ARM_SMMU_EVTQ_IRQ_CFG2:
	case ARM_SMMU_PRIQ_PROD + SZ_64K:
	case ARM_SMMU_PRIQ_CONS + SZ_64K:
	case ARM_SMMU_PRIQ_IRQ_CFG1:
	case ARM_SMMU_PRIQ_IRQ_CFG2:
	case ARM_SMMU_GERRORN:
	case ARM_SMMU_GERROR_IRQ_CFG1:
	case ARM_SMMU_GERROR_IRQ_CFG2:
	case ARM_SMMU_IRQ_CTRLACK:
	case ARM_SMMU_IRQ_CTRL:
	case ARM_SMMU_CR0ACK:
	case ARM_SMMU_CR2:
		/* These are 32 bit registers. */
		WARN_ON(len != sizeof(u32));
		fallthrough;
	case ARM_SMMU_EVTQ_BASE:
	case ARM_SMMU_EVTQ_IRQ_CFG0:
	case ARM_SMMU_PRIQ_BASE:
	case ARM_SMMU_PRIQ_IRQ_CFG0:
	case ARM_SMMU_GERROR_IRQ_CFG0:
		mask = read_write;
		break;
	case ARM_SMMU_IIDR:
	case ARM_SMMU_IDR5:
	case ARM_SMMU_IDR3:
	case ARM_SMMU_IDR1:
	case ARM_SMMU_GERROR:
		WARN_ON(len != sizeof(u32));
		mask = read_only;
	};

	if (WARN_ON(!mask))
		goto out_ret;

	if (is_write) {
		if (len == sizeof(u64))
			writeq_relaxed(regs->regs[rd] & mask, smmu->base + off);
		else
			writel_relaxed(regs->regs[rd] & mask, smmu->base + off);
	} else {
		if (len == sizeof(u64))
			regs->regs[rd] = readq_relaxed(smmu->base + off) & mask;
		else
			regs->regs[rd] = readl_relaxed(smmu->base + off) & mask;
	}

out_ret:
	return true;
}

static bool smmu_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
	struct hyp_arm_smmu_v3_device *smmu;
	bool ret;

	for_each_smmu(smmu) {
		if (addr < smmu->mmio_addr || addr >= smmu->mmio_addr + smmu->mmio_size)
			continue;
		hyp_spin_lock(&smmu->lock);
		ret = smmu_dabt_device(smmu, regs, esr, addr - smmu->mmio_addr);
		hyp_spin_unlock(&smmu->lock);
		return ret;
	}
	return false;
}

static void smmu_host_stage2_idmap(phys_addr_t start, phys_addr_t end, int prot)
{
}

/* Shared with the kernel driver in EL1 */
struct kvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
	.host_stage2_idmap		= smmu_host_stage2_idmap,
	.dabt_handler			= smmu_dabt_handler,
};
