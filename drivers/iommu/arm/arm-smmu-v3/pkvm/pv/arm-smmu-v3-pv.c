// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <asm/kvm_hyp.h>
#include <nvhe/alloc.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>

#include "arm_smmu_v3.h"
#include "arm-smmu-v3-lib-hyp.h"
#include "arm-smmu-v3-module.h"

#include <linux/io-pgtable.h>
#include "../../../io-pgtable-arm.h"

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
/*
 * SMMUv3 domain:
 * @domain: Pointer to the IOMMU domain.
 * @smmu: SMMU owner of the domain
 * @type: Type of domain (S1, S2)
 * @pgt_lock: Lock for page table
 * @pgtable: io_pgtable instance for this domain
 */
struct hyp_arm_smmu_v3_domain {
	struct kvm_hyp_iommu_domain     	*domain;
	struct hyp_arm_smmu_v3_device_pv 	*smmu;
	u32					type;
	hyp_spinlock_t				pgt_lock;
	struct io_pgtable			*pgtable;
};

static struct hyp_arm_smmu_v3_device_pv *smmu_id_to_ptr(pkvm_handle_t smmu_id)
{
	if (smmu_id >= kvm_hyp_arm_smmu_v3_pv_count)
		return NULL;

	smmu_id = array_index_nospec(smmu_id, kvm_hyp_arm_smmu_v3_pv_count);
	return &kvm_hyp_arm_smmu_v3_pv_smmus[smmu_id];
}

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

static void __smmu_add_cmd(void *__opaque, struct arm_smmu_cmdq_batch *unused,
			   struct arm_smmu_cmdq_ent *cmd)
{
	struct hyp_arm_smmu_v3_device *smmu = (struct hyp_arm_smmu_v3_device *)__opaque;

	WARN_ON(smmu_add_cmd(smmu, cmd));
}

static void smmu_inv_domain(struct hyp_arm_smmu_v3_device_pv *smmu,
			    struct hyp_arm_smmu_v3_domain *smmu_domain)
{
	struct kvm_hyp_iommu_domain *domain = smmu_domain->domain;
	struct arm_smmu_cmdq_ent cmd = {};

	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_S1) {
		cmd.opcode = CMDQ_OP_TLBI_NH_ASID;
		cmd.tlbi.asid = domain->domain_id;
	}
	else {
		cmd.opcode = CMDQ_OP_TLBI_S12_VMALL;
		cmd.tlbi.vmid = domain->domain_id;
	}

	WARN_ON(smmu_send_cmd(smmu, &cmd));
}

static void smmu_tlb_flush_all(void *cookie)
{
	struct kvm_hyp_iommu_domain *domain = cookie;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct hyp_arm_smmu_v3_device_pv *smmu = smmu_domain->smmu;

	kvm_smmu_lock(&smmu->common);
	smmu_inv_domain(smmu, smmu_domain);
	kvm_smmu_unlock(&smmu->common);
}

static int smmu_tlb_inv_range_smmu(struct hyp_arm_smmu_v3_device_pv *smmu,
				   struct kvm_hyp_iommu_domain *domain,
				   struct arm_smmu_cmdq_ent *cmd,
				   unsigned long iova, size_t size, size_t granule)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	arm_smmu_tlb_inv_build(cmd, iova, size, granule,
			       smmu_domain->pgtable->cfg.pgsize_bitmap,
			       smmu->common.features & ARM_SMMU_FEAT_RANGE_INV,
			       smmu, __smmu_add_cmd, NULL);
	return smmu_sync_cmd(&smmu->common);
}

static void smmu_tlb_inv_range(struct kvm_hyp_iommu_domain *domain,
			       unsigned long iova, size_t size, size_t granule,
			       bool leaf)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	unsigned long end = iova + size;
	struct arm_smmu_cmdq_ent cmd;
	struct hyp_arm_smmu_v3_device_pv *smmu = smmu_domain->smmu;

	cmd.tlbi.leaf = leaf;
	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_S1) {
		cmd.opcode = CMDQ_OP_TLBI_NH_VA;
		cmd.tlbi.asid = domain->domain_id;
		cmd.tlbi.vmid = 0;
	} else {
		cmd.opcode = CMDQ_OP_TLBI_S2_IPA;
		cmd.tlbi.vmid = domain->domain_id;
	}
	/*
	 * There are no mappings at high addresses since we don't use TTB1, so
	 * no overflow possible.
	 */
	BUG_ON(end < iova);
	kvm_smmu_lock(&smmu->common);
	WARN_ON(smmu_tlb_inv_range_smmu(smmu, domain,
					&cmd, iova, size, granule));
	kvm_smmu_unlock(&smmu->common);
}

static void smmu_tlb_flush_walk(unsigned long iova, size_t size,
				size_t granule, void *cookie)
{
	smmu_tlb_inv_range(cookie, iova, size, granule, false);
}

static void smmu_tlb_add_page(struct iommu_iotlb_gather *gather,
			      unsigned long iova, size_t granule,
			      void *cookie)
{
	if (gather)
		kvm_iommu_iotlb_gather_add_page(cookie, gather, iova, granule);
	else
		smmu_tlb_inv_range(cookie, iova, granule, granule, true);
}

static const struct iommu_flush_ops smmu_tlb_ops = {
	.tlb_flush_all  = smmu_tlb_flush_all,
	.tlb_flush_walk = smmu_tlb_flush_walk,
	.tlb_add_page	= smmu_tlb_add_page,
};

static void smmu_iotlb_sync(struct kvm_hyp_iommu_domain *domain,
			    struct iommu_iotlb_gather *gather)
{
	size_t size;

	if (!gather->pgsize)
		return;
	size = gather->end - gather->start + 1;
	smmu_tlb_inv_range(domain, gather->start, size,  gather->pgsize, true);
}

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

static int smmu_sync_cd(struct hyp_arm_smmu_v3_device_pv *smmu, u64 *cd, u32 sid, u32 ssid)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CFGI_CD,
		.cfgi.sid	= sid,
		.cfgi.ssid	= ssid,
		.cfgi.leaf = true,
	};

	if (!(smmu->common.features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(cd, CTXDESC_CD_DWORDS << 3);

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

static u64 *smmu_get_cd_ptr(u64 *cdtab, u32 ssid)
{
	/* Only linear supported for now. */
	return cdtab + ssid * CTXDESC_CD_DWORDS;
}

static u64 *smmu_alloc_cd(struct hyp_arm_smmu_v3_device *smmu, u32 pasid_bits)
{
	u64 *cd_table;
	u32 requested_order = get_order((1 << pasid_bits) *
					(CTXDESC_CD_DWORDS << 3));

	/*
	 * We support max of 64K linear tables only, this should be enough
	 * for 128 pasids
	 */
	if (WARN_ON(requested_order > 4))
		return NULL;

	cd_table = kvm_iommu_donate_pages(requested_order, 0);
	if (!cd_table)
		return NULL;
	return (u64 *)hyp_virt_to_phys(cd_table);
}

__maybe_unused
static void smmu_free_cd(u64 *cd_table, u32 pasid_bits)
{
	u32 order = get_order((1 << pasid_bits) *
			      (CTXDESC_CD_DWORDS << 3));

	kvm_iommu_reclaim_pages(cd_table, order);
}

static int smmu_domain_finalise(struct hyp_arm_smmu_v3_device_pv *smmu,
				struct kvm_hyp_iommu_domain *domain)
{
	struct io_pgtable_cfg cfg;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	enum io_pgtable_fmt fmt;
	struct io_pgtable_ops *ops;

	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_S1) {
		size_t ias = (smmu->common.features & ARM_SMMU_FEAT_VAX) ? 52 : 48;
		fmt = ARM_64_LPAE_S1;
		cfg = (struct io_pgtable_cfg) {
			.pgsize_bitmap = smmu->common.pgsize_bitmap,
			.ias = min_t(unsigned long, ias, VA_BITS),
			.oas = smmu->common.ias,
			.coherent_walk = smmu->common.features & ARM_SMMU_FEAT_COHERENCY,
			.tlb = &smmu_tlb_ops,
		};
	} else {
		fmt = ARM_64_LPAE_S2;
		cfg = (struct io_pgtable_cfg) {
			.pgsize_bitmap = smmu->common.pgsize_bitmap,
			.ias = smmu->common.ias,
			.oas = smmu->common.oas,
			.coherent_walk = smmu->common.features & ARM_SMMU_FEAT_COHERENCY,
			.tlb = &smmu_tlb_ops,
		};
	}

	ops = kvm_alloc_io_pgtable_ops(fmt, &cfg, domain);
	if (!ops)
		return -ENOMEM;
	smmu_domain->pgtable = io_pgtable_ops_to_pgtable(ops);
	return 0;
}

static int smmu_domain_config_s2(struct kvm_hyp_iommu_domain *domain,
				 struct arm_smmu_ste *ste)
{
	struct io_pgtable_cfg *cfg;
	u64 ts, sl, ic, oc, sh, tg, ps;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	cfg = &smmu_domain->pgtable->cfg;
	ps = cfg->arm_lpae_s2_cfg.vtcr.ps;
	tg = cfg->arm_lpae_s2_cfg.vtcr.tg;
	sh = cfg->arm_lpae_s2_cfg.vtcr.sh;
	oc = cfg->arm_lpae_s2_cfg.vtcr.orgn;
	ic = cfg->arm_lpae_s2_cfg.vtcr.irgn;
	sl = cfg->arm_lpae_s2_cfg.vtcr.sl;
	ts = cfg->arm_lpae_s2_cfg.vtcr.tsz;

	ste->data[0] = cpu_to_le64(STRTAB_STE_0_V |
		FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S2_TRANS));
	ste->data[1] = cpu_to_le64(FIELD_PREP(STRTAB_STE_1_SHCFG, STRTAB_STE_1_SHCFG_INCOMING));
	ste->data[2] = cpu_to_le64(FIELD_PREP(STRTAB_STE_2_VTCR,
			FIELD_PREP(STRTAB_STE_2_VTCR_S2PS, ps) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2TG, tg) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2SH0, sh) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2OR0, oc) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2IR0, ic) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2SL0, sl) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2T0SZ, ts)) |
		 FIELD_PREP(STRTAB_STE_2_S2VMID, domain->domain_id) |
		 STRTAB_STE_2_S2AA64 | STRTAB_STE_2_S2R);
	ste->data[3] = cpu_to_le64(cfg->arm_lpae_s2_cfg.vttbr & STRTAB_STE_3_S2TTB_MASK);

	return 0;
}

static u64 *smmu_domain_config_s1_ste(struct hyp_arm_smmu_v3_device_pv *smmu,
				      u32 pasid_bits, struct arm_smmu_ste *ste)
{
	u64 *cd_table;

	cd_table = smmu_alloc_cd(&smmu->common, pasid_bits);
	if (!cd_table)
		return NULL;

	ste->data[1] = FIELD_PREP(STRTAB_STE_1_S1DSS, STRTAB_STE_1_S1DSS_SSID0) |
		FIELD_PREP(STRTAB_STE_1_S1CIR, STRTAB_STE_1_S1C_CACHE_WBRA) |
		FIELD_PREP(STRTAB_STE_1_S1COR, STRTAB_STE_1_S1C_CACHE_WBRA) |
		FIELD_PREP(STRTAB_STE_1_S1CSH, ARM_SMMU_SH_ISH);
	ste->data[0] = ((u64)cd_table & STRTAB_STE_0_S1CTXPTR_MASK) |
		FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S1_TRANS) |
		FIELD_PREP(STRTAB_STE_0_S1CDMAX, pasid_bits) |
		FIELD_PREP(STRTAB_STE_0_S1FMT, STRTAB_STE_0_S1FMT_LINEAR) |
		STRTAB_STE_0_V;

	return cd_table;
}

/*
 * This function handles configuration for pasid and non-pasid domains
 * with the following assumptions:
 * - pasid 0 always attached first, this should be the typicall flow
 *   for the kernel where attach_dev is always called before set_dev_pasid.
 *   In that case only pasid 0 is allowed to allocate memory for the CD,
 *   and other pasids would expect to find the tabel.
 * - pasid 0 is detached last, also guaranteed from the kernel.
 */
static int smmu_domain_config_s1(struct hyp_arm_smmu_v3_device_pv *smmu,
				 struct kvm_hyp_iommu_domain *domain,
				 u32 sid, u32 pasid, u32 pasid_bits,
				 struct arm_smmu_ste *ste)
{
	struct arm_smmu_ste *dst;
	u64 val;
	u64 *cd_entry, *cd_table;
	struct io_pgtable_cfg *cfg;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	cfg = &smmu_domain->pgtable->cfg;
	dst = smmu_get_ste_ptr(&smmu->common, sid);
	val = dst->data[0];

	if (FIELD_GET(STRTAB_STE_0_CFG, val) == STRTAB_STE_0_CFG_S2_TRANS)
		return -EBUSY;

	if (pasid == 0) {
		/* Must be first attach after a detach. */
		if (val)
			return -EBUSY;

		cd_table = smmu_domain_config_s1_ste(smmu, pasid_bits, ste);
		if (!cd_table)
			return -ENOMEM;
	} else {
		u32 nr_entries;

		cd_table = (u64 *)(FIELD_GET(STRTAB_STE_0_S1CTXPTR_MASK, val) << 6);
		if (!cd_table)
			return -EINVAL;
		nr_entries = 1 << FIELD_GET(STRTAB_STE_0_S1CDMAX, val);
		if (pasid >= nr_entries)
			return -E2BIG;
	}

	/* Write CD. */
	cd_entry = smmu_get_cd_ptr(hyp_phys_to_virt((u64)cd_table), pasid);

	/* CD already used by another device. */
	if (cd_entry[0])
		return -EBUSY;

	cd_entry[1] = cpu_to_le64(cfg->arm_lpae_s1_cfg.ttbr & CTXDESC_CD_1_TTB0_MASK);
	cd_entry[2] = 0;
	cd_entry[3] = cpu_to_le64(cfg->arm_lpae_s1_cfg.mair);

	/* STE is live. */
	if (pasid)
		smmu_sync_cd(smmu, cd_entry, sid, pasid);
	val =  FIELD_PREP(CTXDESC_CD_0_TCR_T0SZ, cfg->arm_lpae_s1_cfg.tcr.tsz) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_TG0, cfg->arm_lpae_s1_cfg.tcr.tg) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_IRGN0, cfg->arm_lpae_s1_cfg.tcr.irgn) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_ORGN0, cfg->arm_lpae_s1_cfg.tcr.orgn) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_SH0, cfg->arm_lpae_s1_cfg.tcr.sh) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_IPS, cfg->arm_lpae_s1_cfg.tcr.ips) |
	       CTXDESC_CD_0_TCR_EPD1 | CTXDESC_CD_0_AA64 |
	       CTXDESC_CD_0_R | CTXDESC_CD_0_A |
	       CTXDESC_CD_0_ASET |
	       FIELD_PREP(CTXDESC_CD_0_ASID, domain->domain_id) |
	       CTXDESC_CD_0_V;
	WRITE_ONCE(cd_entry[0], cpu_to_le64(val));
	/* STE is live. */
	if (pasid)
		smmu_sync_cd(smmu, cd_entry, sid, pasid);
	return 0;
}

static int smmu_attach_dev(pkvm_handle_t iommu, struct kvm_hyp_iommu_domain *domain,
			   u32 sid, u32 pasid, u32 pasid_bits, unsigned long flags)
{
	int i;
	int ret;
	struct arm_smmu_ste *dst;
	struct arm_smmu_ste ste = {};
	struct hyp_arm_smmu_v3_device_pv *smmu = smmu_id_to_ptr(iommu);
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	if (!smmu)
		return -ENODEV;
	if (smmu_domain->smmu != smmu)
		return -EBUSY;

	kvm_smmu_lock(&smmu->common);
	dst = smmu_get_alloc_ste_ptr(&smmu->common, sid);
	if (!dst) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_S1) {
		/*
		 * Allocate and config CD, and update CD if possible.
		 */
		pasid_bits = min(pasid_bits, smmu->ssid_bits);
		ret = smmu_domain_config_s1(smmu, domain, sid, pasid,
					    pasid_bits, &ste);
	} else {
		/* Device already attached or pasid for s2. */
		if (dst->data[0] || pasid) {
			ret = -EBUSY;
			goto out_unlock;
		}
		ret = smmu_domain_config_s2(domain, &ste);
	}
	/* We don't update STEs for pasid domains. */
	if (ret || pasid)
		goto out_unlock;

	/*
	 * The SMMU may cache a disabled STE.
	 * Initialize all fields, sync, then enable it.
	 */
	for (i = 1; i < STRTAB_STE_DWORDS; i++)
		dst->data[i] = ste.data[i];

	ret = smmu_sync_ste(smmu, dst->data, sid);
	if (ret)
		goto out_unlock;

	WRITE_ONCE(dst->data[0], ste.data[0]);
	ret = smmu_sync_ste(smmu, dst->data, sid);
	WARN_ON(ret);

out_unlock:
	kvm_smmu_unlock(&smmu->common);
	return ret;
}

static void smmu_free_domain(struct kvm_hyp_iommu_domain *domain)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	if (smmu_domain->pgtable)
		kvm_arm_io_pgtable_free(smmu_domain->pgtable);

	hyp_free(smmu_domain);
}

/*
 * alloc_domain will only allocate the page table and add the IOMMU to domain
 * tracking.
 * However, it doesn't interact with the STE, that is left for attach_dev.
 */
static int smmu_alloc_domain(pkvm_handle_t iommu,
			     struct kvm_hyp_iommu_domain *domain, int type)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain;
	struct hyp_arm_smmu_v3_device_pv *smmu = smmu_id_to_ptr(iommu);
	int ret;

	if (!smmu)
		return -ENODEV;

	if (type >= KVM_ARM_SMMU_DOMAIN_MAX)
		return -EINVAL;

	smmu_domain = hyp_alloc(sizeof(*smmu_domain));
	if (!smmu_domain)
		return -ENOMEM;

	smmu_domain->domain = domain;
	smmu_domain->type = type;
	smmu_domain->smmu = smmu;
	hyp_spin_lock_init(&smmu_domain->pgt_lock);
	domain->priv = (void *)smmu_domain;

	/* No lock needed, alloc_domain is locked from core code. */
	ret = smmu_domain_finalise(smmu, domain);
	if (ret)
		goto out_free_domain;
	return 0;

out_free_domain:
	hyp_free(smmu_domain);
	return ret;
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

static int smmu_reset_device(struct hyp_arm_smmu_v3_device_pv *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cfgi_cmd = {
		.opcode = CMDQ_OP_CFGI_ALL,
	};
	struct arm_smmu_cmdq_ent tlbi_cmd = {
		.opcode = CMDQ_OP_TLBI_NSNH_ALL,
	};

	/* Invalidate all cached configs and TLBs */
	ret = smmu_write_cr0(&smmu->common, CR0_CMDQEN);
	if (ret)
		return ret;

	ret = smmu_add_cmd(&smmu->common, &cfgi_cmd);
	if (ret)
		goto err_disable_cmdq;

	ret = smmu_add_cmd(&smmu->common, &tlbi_cmd);
	if (ret)
		goto err_disable_cmdq;

	ret = smmu_sync_cmd(&smmu->common);
	if (ret)
		goto err_disable_cmdq;

	/* Enable translation */
	return smmu_write_cr0(&smmu->common, CR0_SMMUEN | CR0_CMDQEN | CR0_ATSCHK | CR0_EVTQEN);

err_disable_cmdq:
	return smmu_write_cr0(&smmu->common, 0);
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

	ret = smmu_init_strtab(&smmu->common);
	if (ret)
		return ret;

	return smmu_reset_device(smmu);
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
	.alloc_domain			= smmu_alloc_domain,
	.free_domain			= smmu_free_domain,
	.iotlb_sync			= smmu_iotlb_sync,
	.attach_dev			= smmu_attach_dev,
};
