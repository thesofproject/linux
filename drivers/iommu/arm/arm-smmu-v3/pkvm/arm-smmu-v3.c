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

/* strtab accessors */
#define strtab_log2size(smmu)	(FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, (smmu)->host_ste_cfg))
#define strtab_size(smmu)	((1 << strtab_log2size(smmu)) * STRTAB_STE_DWORDS * 8)
#define strtab_host_base(smmu)	((smmu)->host_ste_base & STRTAB_BASE_ADDR_MASK)
#define strtab_split(smmu)	(FIELD_GET(STRTAB_BASE_CFG_SPLIT, (smmu)->host_ste_cfg))
#define strtab_l1_size(smmu)	((1 << (strtab_log2size(smmu) - strtab_split(smmu))) * \
				 (sizeof(struct arm_smmu_strtab_l1)))
#define strtab_hyp_base(smmu)	((smmu)->features & ARM_SMMU_FEAT_2_LVL_STRTAB ? \
				 (u64 *)(smmu)->strtab_cfg.l2.l1tab :\
				 (u64 *)(smmu)->strtab_cfg.linear.table)

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

#define cmdq_size(cmdq)	((1 << ((cmdq)->llq.max_n_shift)) * CMDQ_ENT_DWORDS * 8)

/*
 * Wait until @cond is true.
 * Return 0 on success, or -ETIMEDOUT
 */
#define smmu_wait(use_wfe, _cond)					\
({								\
	int __ret = 0;						\
	u64 delay = pkvm_time_get() + ARM_SMMU_POLL_TIMEOUT_US;	\
								\
	while (!(_cond)) {					\
		if (use_wfe)					\
			wfe();					\
		if (pkvm_time_get() >= delay) {			\
			__ret = -ETIMEDOUT;			\
			break;					\
		}						\
	}							\
	__ret;							\
})

static bool is_cmdq_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_CMDQEN, smmu->cr0);
}

static bool is_smmu_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_SMMUEN, smmu->cr0);
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

static void smmu_copy_from_host(struct hyp_arm_smmu_v3_device *smmu,
				void *dst_hyp_va, void *src_hyp_va,
				size_t size)
{
	/* Clean and inval DC as the kernel uses NC mapping. */
	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(src_hyp_va, size);
	memcpy(dst_hyp_va, src_hyp_va, size);
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

static int smmu_abort_gbpa(struct hyp_arm_smmu_v3_device *smmu)
{
	writel_relaxed(GBPA_UPDATE | GBPA_ABORT, smmu->base + ARM_SMMU_GBPA);
	/* Wait till UPDATE is cleared. */
	return smmu_wait(false,
			 readl_relaxed(smmu->base + ARM_SMMU_GBPA) == GBPA_ABORT);
}

static bool smmu_cmdq_has_space(struct arm_smmu_queue *cmdq, u32 n)
{
	struct arm_smmu_ll_queue *llq = &cmdq->llq;

	WRITE_ONCE(llq->cons, readl_relaxed(cmdq->cons_reg));
	return queue_has_space(llq, n);
}

static bool smmu_cmdq_full(struct arm_smmu_queue *cmdq)
{
	struct arm_smmu_ll_queue *llq = &cmdq->llq;

	WRITE_ONCE(llq->cons, readl_relaxed(cmdq->cons_reg));
	return queue_full(llq);
}

static bool smmu_cmdq_empty(struct arm_smmu_queue *cmdq)
{
	struct arm_smmu_ll_queue *llq = &cmdq->llq;

	WRITE_ONCE(llq->cons, readl_relaxed(cmdq->cons_reg));
	return queue_empty(llq);
}

static void smmu_add_cmd_raw(struct hyp_arm_smmu_v3_device *smmu,
			     u64 *cmd)
{
	struct arm_smmu_queue *q = &smmu->cmdq;
	struct arm_smmu_ll_queue *llq = &q->llq;

	queue_write(Q_ENT(q, llq->prod), cmd,  CMDQ_ENT_DWORDS);
	llq->prod = queue_inc_prod_n(llq, 1);
}

static int smmu_add_cmd(struct hyp_arm_smmu_v3_device *smmu,
			struct arm_smmu_cmdq_ent *ent)
{
	int ret;
	u64 cmd[CMDQ_ENT_DWORDS];

	ret = smmu_wait(smmu->features & ARM_SMMU_FEAT_SEV,
			!smmu_cmdq_full(&smmu->cmdq));
	if (ret)
		return ret;

	ret = arm_smmu_cmdq_build_cmd(cmd, ent);
	if (ret)
		return ret;

	smmu_add_cmd_raw(smmu, cmd);
	writel_relaxed(smmu->cmdq.llq.prod, smmu->cmdq.prod_reg);
	return 0;
}

static int smmu_sync_cmd(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CMD_SYNC,
	};

	ret = smmu_add_cmd(smmu, &cmd);
	if (ret)
		return ret;

	return smmu_wait(smmu->features & ARM_SMMU_FEAT_SEV,
			 smmu_cmdq_empty(&smmu->cmdq));
}

__maybe_unused
static int smmu_send_cmd(struct hyp_arm_smmu_v3_device *smmu,
			 struct arm_smmu_cmdq_ent *cmd)
{
	int ret = smmu_add_cmd(smmu, cmd);

	if (ret)
		return ret;

	return smmu_sync_cmd(smmu);
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

/* Get an STE for a stream table base. */
static struct arm_smmu_ste *smmu_get_ste_ptr(struct hyp_arm_smmu_v3_device *smmu,
					     u32 sid, u64 *strtab)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	struct arm_smmu_ste *table = (struct arm_smmu_ste *)strtab;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		struct arm_smmu_strtab_l1 *l1tab = (struct arm_smmu_strtab_l1 *)strtab;
		u32 l1_idx = arm_smmu_strtab_l1_idx(sid);
		struct arm_smmu_strtab_l2 *l2ptr;

		if (WARN_ON(l1_idx >= cfg->l2.num_l1_ents) ||
			!(l1tab[l1_idx].l2ptr & STRTAB_L1_DESC_SPAN))
			return NULL;

		l2ptr = hyp_phys_to_virt(l1tab[l1_idx].l2ptr & STRTAB_L1_DESC_L2PTR_MASK);
		/* Two-level walk */
		return &l2ptr->stes[arm_smmu_strtab_l2_idx(sid)];
	}
	if (WARN_ON(sid >= cfg->linear.num_ents))
		return NULL;
	return &table[sid];
}

static int smmu_shadow_l2_strtab(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	u32 idx = arm_smmu_strtab_l1_idx(sid);
	u64 *host_ste_base = hyp_phys_to_virt(strtab_host_base(smmu));
	struct arm_smmu_strtab_l1 *l1_desc = &smmu->strtab_cfg.l2.l1tab[idx];
	u64 l1_desc_host;
	struct arm_smmu_strtab_l2 *l2table;

	l2table = kvm_iommu_donate_pages(get_order(sizeof(*l2table)));
	if (!l2table)
		return -ENOMEM;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(&host_ste_base[idx], sizeof(*l1_desc));
	l1_desc_host = host_ste_base[idx];

	arm_smmu_write_strtab_l1_desc(l1_desc, hyp_virt_to_phys(l2table));
	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(l1_desc, sizeof(*l1_desc));

	smmu_share_pages(l1_desc_host & STRTAB_L1_DESC_L2PTR_MASK, sizeof(*l2table));
	return 0;
}

static void smmu_reshadow_ste(struct hyp_arm_smmu_v3_device *smmu, u32 sid, bool leaf)
{
	u64 *host_ste_base = hyp_phys_to_virt(strtab_host_base(smmu));
	u64 *hyp_ste_base = strtab_hyp_base(smmu);
	struct arm_smmu_ste *host_ste_ptr = smmu_get_ste_ptr(smmu, sid, host_ste_base);
	struct arm_smmu_ste *hyp_ste_ptr = smmu_get_ste_ptr(smmu, sid, hyp_ste_base);

	/*
	 * Linux only uses leaf = 1, when leaf is 0, we need to verify that this
	 * is a 2 level table and reshadow of l2.
	 * Also Linux never clears l1 ptr, that needs to free the old shadow.
	 */
	if (WARN_ON(!leaf || !host_ste_ptr))
		return;

	/* If host is valid and hyp is not, means a new L1 installed. */
	if (!hyp_ste_ptr) {
		WARN_ON(smmu_shadow_l2_strtab(smmu, sid));
		hyp_ste_ptr = smmu_get_ste_ptr(smmu, sid, hyp_ste_base);
	}

	smmu_copy_from_host(smmu, hyp_ste_ptr->data, host_ste_ptr->data,
			    STRTAB_STE_DWORDS << 3);
}

static int smmu_init_strtab(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	u32 reg;
	enum kvm_pgtable_prot prot = PAGE_HYP;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		prot |= KVM_PGTABLE_PROT_NORMAL_NC;

	ret = ___pkvm_host_donate_hyp_prot(hyp_phys_to_pfn(smmu->strtab_dma),
					   smmu->strtab_size >> PAGE_SHIFT,
					   false, prot);
	if (ret)
		return ret;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		unsigned int last_sid_idx =
			arm_smmu_strtab_l1_idx((1ULL << smmu->sid_bits) - 1);

		cfg->l2.l1tab = hyp_phys_to_virt(smmu->strtab_dma);
		cfg->l2.l1_dma = smmu->strtab_dma;
		cfg->l2.num_l1_ents = min(last_sid_idx + 1, STRTAB_MAX_L1_ENTRIES);

		reg = FIELD_PREP(STRTAB_BASE_CFG_FMT,
				 STRTAB_BASE_CFG_FMT_2LVL) |
		      FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE,
				 ilog2(cfg->l2.num_l1_ents) + STRTAB_SPLIT) |
		      FIELD_PREP(STRTAB_BASE_CFG_SPLIT, STRTAB_SPLIT);
	} else {
		cfg->linear.table = hyp_phys_to_virt(smmu->strtab_dma);
		cfg->linear.ste_dma = smmu->strtab_dma;
		cfg->linear.num_ents = 1UL << smmu->sid_bits;
		reg = FIELD_PREP(STRTAB_BASE_CFG_FMT,
				 STRTAB_BASE_CFG_FMT_LINEAR) |
		      FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE, smmu->sid_bits);
	}

	writeq_relaxed((smmu->strtab_dma & STRTAB_BASE_ADDR_MASK) | STRTAB_BASE_RA,
		       smmu->base + ARM_SMMU_STRTAB_BASE);
	writel_relaxed(reg, smmu->base + ARM_SMMU_STRTAB_BASE_CFG);
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

	ret = smmu_init_strtab(smmu);
	if (ret)
		goto out_ret;

	ret = smmu_abort_gbpa(smmu);
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

static bool smmu_filter_command(struct hyp_arm_smmu_v3_device *smmu, u64 *command)
{
	u64 type = FIELD_GET(CMDQ_0_OP, command[0]);

	switch (type) {
	case CMDQ_OP_CFGI_STE:
	{
		u32 sid = FIELD_GET(CMDQ_CFGI_0_SID, command[0]);
		u32 leaf = FIELD_GET(CMDQ_CFGI_1_LEAF, command[1]);

		smmu_reshadow_ste(smmu, sid, leaf);
		break;
	}
	case CMDQ_OP_CFGI_ALL:
	{
		/*
		 * Linux doesn't use range STE invalidation, and only use this
		 * for CFGI_ALL, which is done on reset and not on an new STE
		 * being used.
		 * Although, this is not architectural we rely on the current Linux
		 * implementation.
		 */
		WARN_ON((FIELD_GET(CMDQ_CFGI_1_RANGE, command[1]) != 31));
		break;
	}
	case CMDQ_OP_TLBI_NH_ASID:
	case CMDQ_OP_TLBI_NH_VA:
	case 0x13: /* CMD_TLBI_NH_VAA: Not used by Linux */
	{
		/* Only allow VMID = 0*/
		if (FIELD_GET(CMDQ_TLBI_0_VMID, command[0]) == 0)
			break;
		break;
	}
	case 0x10: /* CMD_TLBI_NH_ALL: Not used by Linux */
	case CMDQ_OP_TLBI_EL2_ALL:
	case CMDQ_OP_TLBI_EL2_VA:
	case CMDQ_OP_TLBI_EL2_ASID:
	case CMDQ_OP_TLBI_S12_VMALL:
	case 0x23: /* CMD_TLBI_EL2_VAA: Not used by Linux */
		return WARN_ON(true);
	case CMDQ_OP_CMD_SYNC:
		if (FIELD_GET(CMDQ_SYNC_0_CS, command[0]) == CMDQ_SYNC_0_CS_IRQ) {
			/* Allow it, but let the host timeout, as this should never happen. */
			command[0] &= ~CMDQ_SYNC_0_CS;
			command[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_SEV);
			command[1] &= ~CMDQ_SYNC_1_MSIADDR_MASK;
		}
		break;
	}

	return false;
}

static void smmu_emulate_cmdq_insert(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 *host_cmdq = hyp_phys_to_virt(smmu->cmdq_host.q_base & Q_BASE_ADDR_MASK);
	int idx;
	u64 cmd[CMDQ_ENT_DWORDS];
	bool skip;
	u32 space;
	bool use_wfe = smmu->features & ARM_SMMU_FEAT_SEV;

	if (!is_cmdq_enabled(smmu))
		return;

	space = (1 << (smmu->cmdq_host.llq.max_n_shift)) - queue_space(&smmu->cmdq_host.llq);
	/* Wait for the command queue to have some space. */
	WARN_ON(smmu_wait(use_wfe, smmu_cmdq_has_space(&smmu->cmdq, space)));

	while (space--) {
		idx = Q_IDX(&smmu->cmdq_host.llq, smmu->cmdq_host.llq.cons);
		queue_inc_cons(&smmu->cmdq_host.llq);

		smmu_copy_from_host(smmu, cmd, &host_cmdq[idx * CMDQ_ENT_DWORDS],
				    CMDQ_ENT_DWORDS << 3);
		skip = smmu_filter_command(smmu, cmd);
		if (skip)
			continue;
		smmu_add_cmd_raw(smmu, cmd);
	}

	writel_relaxed(smmu->cmdq.llq.prod, smmu->cmdq.prod_reg);

	WARN_ON(smmu_wait(use_wfe, smmu_cmdq_empty(&smmu->cmdq)));
}

static void smmu_update_ste_shadow(struct hyp_arm_smmu_v3_device *smmu, bool enabled)
{
	size_t strtab_size;
	u32 fmt  = FIELD_GET(STRTAB_BASE_CFG_FMT, smmu->host_ste_cfg);

	/* Linux doesn't change the fmt nor size of the strtab in the run time. */
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		strtab_size = strtab_l1_size(smmu);
		WARN_ON(fmt != STRTAB_BASE_CFG_FMT_2LVL);
		WARN_ON((strtab_split(smmu) != STRTAB_SPLIT));
	} else {
		strtab_size = strtab_size(smmu);
		WARN_ON(fmt != STRTAB_BASE_CFG_FMT_LINEAR);
		WARN_ON(FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, smmu->host_ste_cfg) >
		       smmu->sid_bits);
	}

	if (enabled)
		WARN_ON(smmu_share_pages(strtab_host_base(smmu), strtab_size));
	else
		WARN_ON(smmu_unshare_pages(strtab_host_base(smmu), strtab_size));
}

static void smmu_emulate_enable(struct hyp_arm_smmu_v3_device *smmu)
{
	/* Enabling SMMU without CMDQ, means TLB invalidation won't work. */
	WARN_ON(!is_cmdq_enabled(smmu));
	smmu_update_ste_shadow(smmu, true);
}

static void smmu_emulate_disable(struct hyp_arm_smmu_v3_device *smmu)
{
	smmu_update_ste_shadow(smmu, false);
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
	case ARM_SMMU_CMDQ_BASE:
		if (is_write) {
			/* Not allowed by the architecture */
			WARN_ON(is_cmdq_enabled(smmu));
			smmu->cmdq_host.q_base = val;
		} else {
			regs->regs[rd] = smmu->cmdq_host.q_base;
		}
		goto out_ret;
	case ARM_SMMU_CMDQ_PROD:
		if (is_write) {
			smmu->cmdq_host.llq.prod = val;
			smmu_emulate_cmdq_insert(smmu);
		} else {
			regs->regs[rd] = smmu->cmdq_host.llq.prod;
		}
		goto out_ret;
	case ARM_SMMU_CMDQ_CONS:
		if (is_write) {
			/* Not allowed by the architecture */
			WARN_ON(is_cmdq_enabled(smmu));
			smmu->cmdq_host.llq.cons = val;
		} else {
			/* Propagate errors back to the host.*/
			u32 cons = readl_relaxed(smmu->base + ARM_SMMU_CMDQ_CONS);
			u32 err = CMDQ_CONS_ERR & cons;

			regs->regs[rd] = smmu->cmdq_host.llq.cons | err;
		}
		goto out_ret;
	case ARM_SMMU_STRTAB_BASE:
		if (is_write) {
			/* Must only be written when SMMU_CR0.SMMUEN == 0.*/
			WARN_ON(is_smmu_enabled(smmu));
			smmu->host_ste_base = val;
		}
		goto out_ret;
	case ARM_SMMU_STRTAB_BASE_CFG:
		if (is_write) {
			/* Must only be written when SMMU_CR0.SMMUEN == 0.*/
			WARN_ON(is_smmu_enabled(smmu));
			smmu->host_ste_cfg = val;
		}
		goto out_ret;
	case ARM_SMMU_GBPA:
		/* Ignore write, always read to abort. */
		if (!is_write)
			regs->regs[rd] = GBPA_ABORT;

		WARN_ON(len != sizeof(u32));
		goto out_ret;
	case ARM_SMMU_CR0:
		if (is_write) {
			bool last_cmdq_en = is_cmdq_enabled(smmu);
			bool last_smmu_en = is_smmu_enabled(smmu);

			smmu->cr0 = val;
			if (!last_cmdq_en && is_cmdq_enabled(smmu))
				smmu_emulate_cmdq_enable(smmu);
			else if (last_cmdq_en && !is_cmdq_enabled(smmu))
				smmu_emulate_cmdq_disable(smmu);
			if (!last_smmu_en && is_smmu_enabled(smmu))
				smmu_emulate_enable(smmu);
			else if (last_smmu_en && !is_smmu_enabled(smmu))
				smmu_emulate_disable(smmu);
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
