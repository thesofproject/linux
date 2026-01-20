// SPDX-License-Identifier: GPL-2.0
#include <linux/kvm_host.h>
#include <linux/pgtable.h>
#include <asm/kvm_pkvm.h>
#include "early_alloc.h"
#include "gfp.h"
#include "mmu.h"
#include "pgtable.h"

static struct pkvm_pgtable hyp_mmu;
static struct pkvm_pool hyp_mmu_pool;

static struct pkvm_pgtable host_mmu;
pkvm_spinlock_t host_mmu_lock;

static void *hyp_mmu_zalloc_page(void)
{
	return pkvm_alloc_pages(&hyp_mmu_pool, 0);
}

static void hyp_mmu_get_page(void *vaddr)
{
	pkvm_get_page(&hyp_mmu_pool, vaddr);
}

static void hyp_mmu_put_page(void *vaddr)
{
	pkvm_put_page(&hyp_mmu_pool, vaddr);
}

static int hyp_mmu_page_count(void *vaddr)
{
	return pkvm_page_count(vaddr);
}

static const struct pkvm_pgtable_mm_ops hyp_mmu_mm_ops = {
	.zalloc_page = hyp_mmu_zalloc_page,
	.get_page = hyp_mmu_get_page,
	.put_page = hyp_mmu_put_page,
	.page_count = hyp_mmu_page_count,
};

static bool hyp_mmu_pte_present(void *ptep)
{
	return pte_present(*(pte_t *)ptep);
}

static bool hyp_mmu_pte_huge(void *ptep)
{
	return pte_huge(*(pte_t *)ptep);
}

static void hyp_mmu_pte_mkhuge(void *ptep)
{
	pte_t *ptep_ptr = (pte_t *)ptep;

	*ptep_ptr = pte_mkhuge(*ptep_ptr);
}

static unsigned long hyp_mmu_pte_to_phys(void *ptep)
{
	return native_pte_val(*(pte_t *)ptep) & PTE_PFN_MASK;
}

static u64 hyp_mmu_pte_to_prot(void *ptep)
{
	return (u64)pte_flags(pte_clear_flags(*(pte_t *)ptep, _PAGE_PSE));
}

static int hyp_mmu_vaddr_to_index(unsigned long vaddr, int level)
{
	return (vaddr >> page_level_shift(level)) & ((1UL << PTE_SHIFT) - 1);
}

static unsigned long hyp_mmu_level_to_size(int level)
{
	return page_level_size(level);
}

static u64 hyp_mmu_level_to_mask(int level)
{
	return page_level_mask(level);
}

static bool hyp_mmu_pte_is_leaf(void *ptep, int level)
{
	return level == PG_LEVEL_4K || !hyp_mmu_pte_present(ptep) || hyp_mmu_pte_huge(ptep);
}

static int hyp_mmu_pte_size(int level)
{
	return PAGE_SIZE / PTRS_PER_PTE;
}

static int hyp_mmu_pte_count(int level)
{
	return PTRS_PER_PTE;
}

static void hyp_mmu_pte_set(void *ptep, u64 pte)
{
	native_set_pte((pte_t *)ptep, native_make_pte(pte));
}

static u64 hyp_mmu_pte_get(void *ptep)
{
	return READ_ONCE(*(pte_t *)ptep).pte;
}

static void hyp_mmu_flush_tlb(struct pkvm_pgtable *pgt,
			      unsigned long vaddr, unsigned long size)
{
	/*
	 * The pKVM hypervisor will map all valid memory in its MMU during the
	 * init phase and won't change or unmap at the running time. So there is
	 * no usages to change a present leaf in the hyp_mmu. Put a BUG here in
	 * case anything is missed.
	 */
	BUG();
}

static const struct pkvm_pgtable_ops hyp_mmu_pgt_ops = {
	.pte_present = hyp_mmu_pte_present,
	.pte_huge = hyp_mmu_pte_huge,
	.pte_mkhuge = hyp_mmu_pte_mkhuge,
	.pte_to_phys = hyp_mmu_pte_to_phys,
	.pte_to_prot = hyp_mmu_pte_to_prot,
	.vaddr_to_index = hyp_mmu_vaddr_to_index,
	.level_to_size = hyp_mmu_level_to_size,
	.level_to_mask = hyp_mmu_level_to_mask,
	.pte_is_leaf = hyp_mmu_pte_is_leaf,
	.pte_size = hyp_mmu_pte_size,
	.pte_count = hyp_mmu_pte_count,
	.pte_set = hyp_mmu_pte_set,
	.pte_get = hyp_mmu_pte_get,
	.flush_tlb = hyp_mmu_flush_tlb,
};

static int fix_hyp_mmu_refcnt_walker(struct pkvm_pgtable_visit_ctx *ctx,
				     unsigned long walk_flags,
				     void *const arg)
{
	if (!ctx->pgt->pgt_ops->pte_present(ctx->ptep))
		return 0;

	/*
	 * Fix-up the refcount for the page-table pages as the early allocator
	 * was unable to access the pkvm_vmemmap and so the buddy allocator has
	 * initialized the refcount to '1'.
	 */
	hyp_mmu_get_page(ctx->ptep);

	return 0;
}

static int fix_hyp_mmu_page_refcnt(void)
{
	struct pkvm_pgtable_walker walker = {
		.cb = fix_hyp_mmu_refcnt_walker,
		.arg = NULL,
		.walk_flags = PKVM_PGTABLE_WALK_LEAF | PKVM_PGTABLE_WALK_TABLE_POST,
	};
	unsigned long size;

#ifdef CONFIG_PKVM_X86_DEBUG
	/*
	 * Only the memory addresses under VMALLOC_START are mapped by the
	 * pKVM hypervisor itself, thus only need to fix vmmemap for this
	 * range.
	 */
	size = VMALLOC_START & ~hyp_mmu.pgt_ops->level_to_mask(hyp_mmu.cap.level + 1);
#else
	/*
	 * Calculate the max address space, then walk the [0, size) address
	 * range to fixup refcount of every page-table page.
	 */
	size = pkvm_pgtable_max_size(&hyp_mmu);
#endif

	return pkvm_pgtable_walk(&hyp_mmu, 0, size, &walker);
}

int pkvm_hyp_mmu_init(void *pool_base, unsigned long pool_pages)
{
	struct pkvm_pgtable_cap cap = {
		.level = pgtable_l5_enabled() ? 5 : 4,
		.allowed_pgsz = (1 << PG_LEVEL_2M) | (1 << PG_LEVEL_4K),
		.table_prot = (u64)_KERNPG_TABLE_NOENC,
	};

	if (boot_cpu_has(X86_FEATURE_GBPAGES))
		cap.allowed_pgsz |= 1 << PG_LEVEL_1G;

	pkvm_early_alloc_init(pool_base, pool_pages << PAGE_SHIFT);

	return pkvm_pgtable_init(&hyp_mmu, cap, &pkvm_early_alloc_mm_ops, &hyp_mmu_pgt_ops);
}

int pkvm_hyp_mmu_switch_to_buddy(void *pool_base, unsigned long pool_pages)
{
	unsigned long reserved_pages;
	int ret;

	/* Get the early alloc used pages and reserve them in hyp_mmu_pool */
	if (hyp_mmu.mm_ops != &pkvm_early_alloc_mm_ops)
		return -EINVAL;

	reserved_pages = pkvm_early_alloc_nr_used_pages();
	/* Enable buddy allocator */
	ret = pkvm_pool_init(&hyp_mmu_pool, __pkvm_pa(pool_base) >> PAGE_SHIFT,
			     pool_pages, reserved_pages);
	if (ret)
		return ret;

	if (reserved_pages) {
		/*
		 * As the early alloc mm_ops was used to allocate mmu
		 * page-table pages, the refcount of each page-table pages
		 * was not maintained at that time. This should be fixed.
		 */
		ret = fix_hyp_mmu_page_refcnt();
		if (ret)
			return ret;
	}

	hyp_mmu.mm_ops = &hyp_mmu_mm_ops;
	return 0;
}

void pkvm_hyp_mmu_load(void)
{
	native_write_cr3(hyp_mmu.root_pa);
}

int pkvm_hyp_mmu_finalize(hyp_mmu_finalize_fn_t fn)
{
	return fn ? fn(&hyp_mmu) : 0;
}

int pkvm_hyp_mmu_map(unsigned long vaddr, unsigned long phys,
		     unsigned long size, u64 prot)
{
	return pkvm_pgtable_map(&hyp_mmu, vaddr, phys, size, prot);
}

#ifdef CONFIG_PKVM_X86_DEBUG
void pkvm_hyp_mmu_clone_host(unsigned long start_vaddr)
{
	int i = hyp_mmu_vaddr_to_index(start_vaddr, hyp_mmu.cap.level);
	u64 *host_cr3 = __pkvm_va(__native_read_cr3() & PAGE_MASK);
	u64 *ptep = __pkvm_va(hyp_mmu.root_pa);

	for (; i < PTRS_PER_PTE; i++)
		ptep[i] = host_cr3[i];
}
#endif

int pkvm_host_mmu_init(void *pool_base, unsigned long pool_pages, host_mmu_init_fn_t fn)
{
	struct memblock_region *reg;
	unsigned long phys;
	int ret, i;

	pkvm_spin_lock_init(&host_mmu_lock);

	if (!fn)
		return -EOPNOTSUPP;

	ret = fn(&host_mmu, pool_base, pool_pages);
	if (ret)
		return ret;

	/* Map memory blocks with RWX permissions */
	for (i = 0; i < pkvm_memblock_nr; i++) {
		reg = &pkvm_memory[i];
		ret = pkvm_host_mmu_map((unsigned long)reg->base,
					(unsigned long)reg->size,
					true, true, true, false);
		if (ret)
			return ret;
	}

	/* Map holes between memblocks as MMIO with RWX permissions */
	for (i = phys = 0; i < pkvm_memblock_nr; i++, phys = reg->base + reg->size) {
		reg = &pkvm_memory[i];
		ret = pkvm_host_mmu_map(phys, (unsigned long)reg->base - phys,
					true, true, true, true);
		if (ret)
			return ret;
	}

	return 0;
}

int pkvm_host_mmu_finalize(host_mmu_finalize_fn_t fn)
{
	return fn ? fn(&host_mmu) : 0;
}

int pkvm_host_mmu_map(unsigned long phys, unsigned long size,
		      bool read, bool write, bool exec, bool mmio)
{
	u64 prot = host_mmu.pgt_ops->calc_pte_perm(read, write, exec) |
		   host_mmu.pgt_ops->calc_pte_memtype(mmio);

	/* The vaddr == phys for the host MMU */
	return pkvm_pgtable_map(&host_mmu, phys, phys, size, prot);
}

int pkvm_host_mmu_unmap(unsigned long vaddr, unsigned long size)
{
	/* The vaddr == phys for the host MMU */
	return pkvm_pgtable_unmap(&host_mmu, vaddr, vaddr, size);
}
