// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU operations for pKVM
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>
#include <asm/kvm_hypevents.h>

#include <hyp/adjust_pc.h>

#include <linux/iommu.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/spinlock.h>

/* Only one set of ops supported */
struct kvm_iommu_ops *kvm_iommu_ops;

/* Protected by host_mmu.lock */
static bool kvm_idmap_initialized;
static struct hyp_pool iommu_pages_pool;

static inline int pkvm_to_iommu_prot(enum kvm_pgtable_prot prot)
{
	int iommu_prot = 0;

	if (prot & KVM_PGTABLE_PROT_R)
		iommu_prot |= IOMMU_READ;
	if (prot & KVM_PGTABLE_PROT_W)
		iommu_prot |= IOMMU_WRITE;
	if (prot == PKVM_HOST_MMIO_PROT)
		iommu_prot |= IOMMU_MMIO;

	/* We don't understand that, might be dangerous. */
	WARN_ON(prot & ~PKVM_HOST_MEM_PROT);
	return iommu_prot;
}

static int __snapshot_host_stage2(const struct kvm_pgtable_visit_ctx *ctx,
				  enum kvm_pgtable_walk_flags visit)
{
	u64 start = ctx->addr;
	kvm_pte_t pte = *ctx->ptep;
	u32 level = ctx->level;
	u64 end = start + kvm_granule_size(level);
	int prot = IOMMU_READ | IOMMU_WRITE;

	/* Keep unmapped. */
	if (pte && !kvm_pte_valid(pte))
		return 0;

	if (kvm_pte_valid(pte))
		prot = pkvm_to_iommu_prot(kvm_pgtable_stage2_pte_prot(pte));
	else if (!addr_is_memory(start))
		prot |= IOMMU_MMIO;

	kvm_iommu_ops->host_stage2_idmap(start, end, prot);
	return 0;
}

static int kvm_iommu_snapshot_host_stage2(void)
{
	int ret;
	struct kvm_pgtable_walker walker = {
		.cb	= __snapshot_host_stage2,
		.flags	= KVM_PGTABLE_WALK_LEAF,
	};
	struct kvm_pgtable *pgt = &host_mmu.pgt;

	hyp_spin_lock(&host_mmu.lock);
	ret = kvm_pgtable_walk(pgt, 0, BIT(pgt->ia_bits), &walker);
	/* Start receiving calls to host_stage2_idmap. */
	kvm_idmap_initialized = !ret;
	hyp_spin_unlock(&host_mmu.lock);

	return ret;
}

int kvm_iommu_init(void *pool_base, size_t nr_pages)
{
	int ret;

	if (!kvm_iommu_ops || !kvm_iommu_ops->init ||
	    !kvm_iommu_ops->host_stage2_idmap)
		return -ENODEV;

	if (nr_pages) {
		ret = hyp_pool_init(&iommu_pages_pool, hyp_virt_to_pfn(pool_base),
				    nr_pages, 0);
		if (ret)
			return ret;
	}

	ret = kvm_iommu_ops->init();
	if (ret)
		return ret;
	return kvm_iommu_snapshot_host_stage2();
}

void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot)
{
	hyp_assert_lock_held(&host_mmu.lock);

	if (!kvm_idmap_initialized)
		return;
	trace_iommu_idmap(start, end, prot);
	kvm_iommu_ops->host_stage2_idmap(start, end, pkvm_to_iommu_prot(prot));
}

void *kvm_iommu_donate_pages(u8 order)
{
	return hyp_alloc_pages(&iommu_pages_pool, order);
}

void kvm_iommu_reclaim_pages(void *ptr)
{
	hyp_put_page(&iommu_pages_pool, ptr);
}

bool kvm_iommu_host_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
	if (kvm_iommu_ops && kvm_iommu_ops->dabt_handler &&
	    kvm_iommu_ops->dabt_handler(regs, esr, addr)) {
		/* DABT handled by the driver, skip to next instruction. */
		kvm_skip_host_instr();
		return true;
	}
	return false;
}

void kvm_iommu_host_stage2_idmap_complete(bool map)
{
	if (!kvm_idmap_initialized ||
	    !kvm_iommu_ops->host_stage2_idmap_complete)
		return;

	trace_iommu_idmap_complete(map);
	kvm_iommu_ops->host_stage2_idmap_complete(map);
}
