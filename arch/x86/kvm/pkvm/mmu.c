// SPDX-License-Identifier: GPL-2.0
#include <linux/kvm_host.h>
#include <linux/pgtable.h>
#include <asm/kvm_pkvm.h>
#include "early_alloc.h"
#include "gfp.h"
#include "mmu.h"
#include "pgtable.h"
#include "pkvm.h"

static struct pkvm_pgtable hyp_mmu;
static struct pkvm_pool hyp_mmu_pool;

static struct pkvm_pgtable host_mmu;
pkvm_spinlock_t host_mmu_lock;

static const struct pkvm_pgtable_ops *guest_mmu_pgt_ops;
static struct pkvm_pgtable_cap guest_mmu_pgt_cap;

static DEFINE_PER_CPU(struct pkvm_vm *, __current_vm);
#define current_vm (*this_cpu_ptr(&__current_vm))

static void *hyp_mmu_zalloc_page(struct pkvm_memcache *mc)
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

static bool hyp_mmu_pte_annotated(void *ptep)
{
	/* Hypervisor mmu is not used to store annotations */
	return false;
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
	.pte_annotated = hyp_mmu_pte_annotated,
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

static void set_host_mem_pgstate(unsigned long phys, unsigned long size,
				 enum pkvm_page_state pgstate)
{
	for_each_pkvm_page(page, phys, size)
		page->host_state = pgstate;
}

static int check_host_mem_pgstate(unsigned long phys, unsigned long size,
				  enum pkvm_page_state pgstate)
{
	if (!is_memory_range(phys, size))
		return -EINVAL;

	for_each_pkvm_page(page, phys, size) {
		if (page->host_state != pgstate)
			return -EPERM;
	}

	return 0;
}

struct page_ownership {
	const enum pkvm_owner_id *owner;
	const enum pkvm_page_state *state;
};

static int check_page_ownership_walker(struct pkvm_pgtable_visit_ctx *ctx,
				       unsigned long walk_flags,
				       void *const arg)
{
	struct pkvm_pgtable *pgt = ctx->pgt;
	struct page_ownership *expected = arg;
	void *ptep = ctx->ptep;

	if (expected->owner) {
		/* The pte should be a non-present entry to contain owner id. */
		if (pgt->pgt_ops->pte_present(ptep))
			return -EPERM;

		if (*expected->owner != pkvm_pte_owner_id(pgt, ptep))
			return -EPERM;
	}

	if (expected->state && (*expected->state != pkvm_pte_pgstate(pgt, ptep)))
		return -EPERM;

	return 0;
}

static int check_page_owner(struct pkvm_pgtable *pgt, unsigned long vaddr,
			    unsigned long size, const enum pkvm_owner_id expected_owner)
{
	struct page_ownership expected_ownership = {
		.owner = &expected_owner,
		.state = NULL,
	};
	struct pkvm_pgtable_walker walker = {
		.cb = check_page_ownership_walker,
		.arg = &expected_ownership,
		.walk_flags = PKVM_PGTABLE_WALK_LEAF,
	};

	return pkvm_pgtable_walk(pgt, vaddr, size, &walker);
}

static int check_page_owner_and_state(struct pkvm_pgtable *pgt, unsigned long vaddr,
				      unsigned long size, const enum pkvm_owner_id expected_owner,
				      const enum pkvm_page_state expected_state)
{
	struct page_ownership expected_ownership = {
		.owner = &expected_owner,
		.state = &expected_state,
	};
	struct pkvm_pgtable_walker walker = {
		.cb = check_page_ownership_walker,
		.arg = &expected_ownership,
		.walk_flags = PKVM_PGTABLE_WALK_LEAF,
	};

	return pkvm_pgtable_walk(pgt, vaddr, size, &walker);
}

static u64 host_mmu_pte_prot(bool mmio)
{
	return host_mmu.pgt_ops->calc_pte_perm(true, true, true) |
	       host_mmu.pgt_ops->calc_pte_memtype(mmio);
}

static int fix_host_mmu_pgstate_walker(struct pkvm_pgtable_visit_ctx *ctx,
				       unsigned long walk_flags,
				       void *const arg)
{
	const struct pkvm_pgtable_ops *pgt_ops = ctx->pgt->pgt_ops;
	u64 pte;

	if (pgt_ops->pte_present(ctx->ptep)) {
		/* A present pte could map either memory pages or MMIO pages. */
		unsigned long phys, size;

		phys = pgt_ops->pte_to_phys(ctx->ptep);
		size = pgt_ops->level_to_size(ctx->level);

		if (is_mmio_range(phys, size)) {
			/*
			 * Store PKVM_PAGE_OWNED page state in pte for the MMIO
			 * pages. No need to flush TLB as the page state bits in
			 * pte are ignored bits.
			 */
			pte = pgt_ops->pte_get(ctx->ptep) |
			      pgt_ops->pte_mk_pgstate(PKVM_PAGE_OWNED);

			pgt_ops->pte_set(ctx->ptep, pte);
		} else {
			/*
			 * Not MMIO pages then should be memory pages. Otherwise
			 * it is a code bug.
			 */
			BUG_ON(!is_memory_range(phys, size));
			set_host_mem_pgstate(phys, size, PKVM_PAGE_OWNED);
		}
	} else {
		/*
		 * A non-present pte will be used by the pKVM hypervisor to
		 * install MMIO mappings at runtime. Set the page state
		 * as PKVM_PAGE_NONE to as it is non-present leaf. Meanwhile set
		 * the owner ID as PKVM_ID_HYP so that only the pKVM hypervisor
		 * can initiate the transition of this MMIO page to the host.
		 */
		pte = pgt_ops->pte_mk_pgstate(PKVM_PAGE_NONE) |
		      pkvm_pte_mk_owner_id(PKVM_ID_HYP);

		pgt_ops->pte_set(ctx->ptep, pte);
	}

	return 0;
}

static int fix_host_mmu_pgstate(void)
{
	struct pkvm_pgtable_walker walker = {
		.cb = fix_host_mmu_pgstate_walker,
		.arg = NULL,
		.walk_flags = PKVM_PGTABLE_WALK_LEAF,
	};

	return pkvm_pgtable_walk(&host_mmu, 0, pkvm_pgtable_max_size(&host_mmu),
				 &walker);
}

static int host_mmu_map(unsigned long phys, unsigned long size, bool mmio)
{
	/* The vaddr == phys for the host MMU */
	return pkvm_pgtable_map(&host_mmu, phys, phys, size,
				host_mmu_pte_prot(mmio), NULL);
}

static void *guest_mmu_zalloc_page(struct pkvm_memcache *mc)
{
	struct pkvm_page *p;
	void *page;

	page = pkvm_alloc_pages(&current_vm->mmu_pool, 0);
	if (page)
		return page;

	if (!mc)
		return NULL;

	page = pop_pkvm_memcache_page(mc, pkvm_phys_to_virt);
	if (!page)
		return page;

	memset(page, 0, PAGE_SIZE);
	p = pkvm_virt_to_page(page);
	pkvm_set_page_refcounted(p);

	return page;
}

static void guest_mmu_get_page(void *vaddr)
{
	pkvm_get_page(&current_vm->mmu_pool, vaddr);
}

static void guest_mmu_put_page(void *vaddr)
{
	pkvm_put_page(&current_vm->mmu_pool, vaddr);
}

static int guest_mmu_page_count(void *vaddr)
{
	return pkvm_page_count(vaddr);
}

static const struct pkvm_pgtable_mm_ops guest_mmu_mm_ops = {
	.zalloc_page = guest_mmu_zalloc_page,
	.get_page = guest_mmu_get_page,
	.put_page = guest_mmu_put_page,
	.page_count = guest_mmu_page_count,
};

static void drain_pool(struct pkvm_pool *pool, struct pkvm_memcache *host_mc)
{
	struct pkvm_page *page;
	void *p;

	p = pkvm_alloc_pages(pool, 0);
	while (p) {
		page = pkvm_virt_to_page(p);

		/* Don't expect the pool to have greater order pages. */
		WARN_ON(page->order);

		pkvm_page_ref_dec(page);

		push_pkvm_memcache_page(host_mc, p, pkvm_virt_to_host_gpa);

		/*
		 * Pages stored in pool are zeroed by __pkvm_attach_page so do
		 * not repeat this step before donation.
		 */
		pkvm_hyp_donate_host(__pkvm_pa(p), PAGE_SIZE, false);

		p = pkvm_alloc_pages(pool, 0);
	}
}

static void *admit_host_page(void *arg)
{
	struct pkvm_memcache *host_mc = arg;
	void *page;

	page = pop_pkvm_memcache_page(host_mc, pkvm_host_gpa_to_virt);
	if (!page)
		return NULL;

	/*
	 * The page will be cleared by zalloc_page when allocating it from
	 * the memcache, thus no need to clear it now.
	 */
	if (WARN_ON(pkvm_host_donate_hyp(__pkvm_pa(page), PAGE_SIZE, false))) {
		push_pkvm_memcache_page(host_mc, page, pkvm_virt_to_host_gpa);
		return NULL;
	}

	return page;
}

/* Refill our local memcache by popping pages from the one provided by the host. */
static int refill_memcache(struct pkvm_memcache *mc, unsigned long min_pages,
			   struct pkvm_memcache *host_mc)
{
	return topup_pkvm_memcache(mc, min_pages, admit_host_page,
				   pkvm_virt_to_phys, host_mc);
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
	return pkvm_pgtable_map(&hyp_mmu, vaddr, phys, size, prot, NULL);
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
		ret = host_mmu_map((unsigned long)reg->base,
				   (unsigned long)reg->size, false);
		if (ret)
			return ret;

	}

	/* Map holes between memblocks as MMIO with RWX permissions */
	for (i = phys = 0; i < pkvm_memblock_nr; i++, phys = reg->base + reg->size) {
		reg = &pkvm_memory[i];
		ret = host_mmu_map(phys, (unsigned long)reg->base - phys, true);
		if (ret)
			return ret;
	}

	return fix_host_mmu_pgstate();
}

int pkvm_host_mmu_finalize(host_mmu_finalize_fn_t fn)
{
	return fn ? fn(&host_mmu) : 0;
}

void pkvm_guest_mmu_setup(const struct pkvm_pgtable_ops *pgt_ops,
			  struct pkvm_pgtable_cap pgt_cap)
{
	guest_mmu_pgt_ops = pgt_ops;
	guest_mmu_pgt_cap = pgt_cap;
}

int pkvm_guest_mmu_init(struct pkvm_vm *pkvm_vm, phys_addr_t pgd_pa)
{
	struct kvm_pkvm_vm *shared_pkvm = &pkvm_vm->shared_kvm->arch.pkvm;
	int ret;

	if (!PAGE_ALIGNED(pgd_pa))
		return -EINVAL;

	/*
	 * The donated page will be cleared when allocating it from the pool,
	 * before using it for the root pgd. Thus no need to clear it now.
	 */
	ret = pkvm_host_donate_hyp(pgd_pa, PAGE_SIZE, false);
	if (ret)
		return ret;

	ret = pkvm_pool_init(&pkvm_vm->mmu_pool, pkvm_phys_to_pfn(pgd_pa), 1, 0);
	if (ret)
		goto undonate;

	pkvm_spin_lock_init(&pkvm_vm->mmu_lock);

	current_vm = pkvm_vm;
	ret = pkvm_pgtable_init(&pkvm_vm->mmu, guest_mmu_pgt_cap,
				&guest_mmu_mm_ops, guest_mmu_pgt_ops);
	current_vm = NULL;
	if (ret)
		goto undonate;

	init_pkvm_mmu_memcache(&shared_pkvm->guest_mmu_teardown_mc);

	return 0;

undonate:
	pkvm_hyp_donate_host(pgd_pa, PAGE_SIZE, false);
	return ret;
}

void pkvm_guest_mmu_destroy(struct pkvm_vm *pkvm_vm)
{
	struct kvm_pkvm_vm *shared_pkvm = &pkvm_vm->shared_kvm->arch.pkvm;

	current_vm = pkvm_vm;
	pkvm_pgtable_destroy(&pkvm_vm->mmu);
	current_vm = NULL;

	/*
	 * Drain per VM pool after destroying the page-table so the pool
	 * contains all pages freed during that step.
	 */
	drain_pool(&pkvm_vm->mmu_pool, &shared_pkvm->guest_mmu_teardown_mc);
}

int pkvm_guest_mmu_refill_memcache(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_vcpu *vcpu = &pkvm_vcpu->vcpu;
	struct pkvm_memcache *host_mc;

	host_mc = &pkvm_vcpu->shared_vcpu->arch.pkvm.guest_mmu_memcache;

	return refill_memcache(&vcpu->arch.pkvm.guest_mmu_memcache,
			       host_mc->count, host_mc);
}

void pkvm_guest_mmu_free_memcache(struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm_pkvm_vm *shared_pkvm = &pkvm_vcpu->pkvm_vm->shared_kvm->arch.pkvm;
	struct pkvm_memcache *vcpu_mc = &pkvm_vcpu->vcpu.arch.pkvm.guest_mmu_memcache;
	void *addr;

	while (vcpu_mc->count) {
		/* Drain hyp owned memcache and push pages to the teardown memcache */
		addr = pop_pkvm_memcache_page(vcpu_mc, pkvm_phys_to_virt);

		/*
		 * Since pages comes from non-used memcache, there is no need to
		 * zero them before pushing to teardown_mc (which host can
		 * access after donating them back to the host in next step).
		 *
		 * Assume that the host frees all vCPUs sequentially, so no need
		 * to take a lock to protect the host's guest_mmu_teardown_mc from
		 * concurrent access.
		 */
		push_pkvm_memcache_page(&shared_pkvm->guest_mmu_teardown_mc, addr,
					pkvm_virt_to_host_gpa);
		pkvm_hyp_donate_host(__pkvm_pa(addr), PAGE_SIZE, false);
	}
}

/**
 * pkvm_host_donate_hyp() - Donate memory pages from host to hypervisor.
 * @phys:	Physical address of the memory region to donate.
 * @size:	Size of the memory region to donate.
 * @clear:	If true, clear the memory region after donating.
 *
 * The donation transfers ownership of the memory pages in range
 * [@phys, @phys + @size) from the host to the hypervisor, thus protecting them
 * from accessing by the host. The @phys and @size are required to be PAGE_SIZE
 * aligned (@size is also required to be non-zeroed value).
 *
 * The donated memory pages are annotated with PKVM_ID_HYP owner id in the host
 * mmu, and page states are updated to PKVM_PAGE_NONE, to indicate the ownership
 * has been transferred to the hypervisor.
 *
 * Returns: 0 on success, or a negative error code on failure.
 */
int pkvm_host_donate_hyp(unsigned long phys, unsigned long size, bool clear)
{
	int ret;

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size) || size == 0)
		return -EINVAL;

	pkvm_host_mmu_lock();

	ret = check_host_mem_pgstate(phys, size, PKVM_PAGE_OWNED);
	if (ret)
		goto unlock;

	/* The vaddr == phys for the host MMU. */
	ret = pkvm_pgtable_set_owner(&host_mmu, phys, size, PKVM_ID_HYP);
	/*
	 * pkvm_pgtable_set_owner() shouldn't fail here unless there is a bug.
	 * Furthermore, if it fails, it means some (maybe not all) pages in the
	 * range remain mapped in the host mmu, whereas their state will be
	 * changed to PKVM_PAGE_NONE below, causing an inconsistency between the
	 * page state and the mapping, which may lead to unexpected behavior. So
	 * panic if it fails.
	 */
	BUG_ON(ret);

	set_host_mem_pgstate(phys, size, PKVM_PAGE_NONE);
unlock:
	pkvm_host_mmu_unlock();

	if (!ret && clear) {
		/*
		 * No need to flush CPU cache, like what pkvm_clear_memory()
		 * does, as the pKVM hypervisor doesn't access memory via
		 * non-coherent DMA (actually there is no DMA in the pKVM
		 * hypervisor).
		 */
		memset(__pkvm_va(phys), 0, size);
	}

	return ret;
}

/**
 * pkvm_hyp_donate_host() - Donate memory pages from hypervisor to host.
 * @phys:	Physical address of the memory region to donate.
 * @size:	Size of the memory region to donate.
 * @clear:	If true, clear the memory region before donating.
 *
 * The donation transfers ownership of the memory pages in range
 * [@phys, @phys + @size) from the hypervisor to the host, thus allowing the
 * host to access those pages. The @phys and @size are required to be PAGE_SIZE
 * aligned (@size is also required to be non-zeroed value).
 *
 * The donated memory pages are mapped in the host mmu with read/write/exec
 * permission and WB memory type in the host mmu, with the page states being
 * updated to PKVM_PAGE_OWNED to indicate the ownership has been transferred to
 * the host.
 */
void pkvm_hyp_donate_host(unsigned long phys, unsigned long size, bool clear)
{
	void *va = __pkvm_va(phys);
	int ret;

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size) || size == 0) {
		ret = -EINVAL;
		goto out;
	}

	if (clear)
		pkvm_clear_memory(va, size);

	pkvm_host_mmu_lock();

	ret = check_host_mem_pgstate(phys, size, PKVM_PAGE_NONE);
	if (ret)
		goto unlock;

	/* The vaddr == phys for the host MMU */
	ret = check_page_owner(&host_mmu, phys, size, PKVM_ID_HYP);
	if (ret)
		goto unlock;

	/*
	 * pkvm_pgtable_map() shouldn't fail here unless there is a bug.
	 * Furthermore, if it fails, it means some (maybe not all) pages in the
	 * range remain not mapped in the host mmu, whereas their state will be
	 * changed to PKVM_PAGE_OWNED below, causing an inconsistency between
	 * the page state and the mapping, which may lead to unexpected
	 * behavior. So panic if it fails.
	 */
	BUG_ON(ret = pkvm_pgtable_map(&host_mmu, phys, phys, size,
				      host_mmu_pte_prot(false), NULL));

	set_host_mem_pgstate(phys, size, PKVM_PAGE_OWNED);
unlock:
	pkvm_host_mmu_unlock();
out:
	/*
	 * pkvm_hyp_donate_host() is only used by the pKVM hypervisor itself,
	 * not on the host behalf, so it is supposed to be called with correct
	 * parameters, and only for pages that are known to be owned by the
	 * hypervisor. So any error here means a pKVM bug.
	 */
	BUG_ON(ret);
}

/**
 * pkvm_hyp_donate_host_mmio_locked() - Donate MMIO pages from hypervisor to
 *					host with the host mmu already locked
 *					by the caller.
 * @phys:	Physical address of the MMIO region to donate.
 * @size:	Size of the MMIO region to donate.
 *
 * The donation transfers ownership of the MMIO pages in range
 * [@phys, @phys + @size) from the hypervisor to the host, thus allowing the
 * host to access. This expects the caller has already locked the host mmu. The
 * @phys and @size are required to be PAGE_SIZE aligned (@size is also required
 * to be non-zeroed value) to make sure the caller aware only PAGE_SIZE aligned
 * memory range can be donated.
 *
 * The donated MMIO pages are mapped in the host mmu with read/write/exec
 * permission and UC memory type in the host mmu, with the page states being
 * updated to PKVM_PAGE_OWNED to indicate the ownership has been transferred to
 * the host.
 *
 * Returns: 0 on success, or a negative error code on failure.
 */
int pkvm_hyp_donate_host_mmio_locked(unsigned long phys, unsigned long size)
{
	u64 prot = host_mmu.pgt_ops->pte_mk_pgstate(PKVM_PAGE_OWNED) |
		   host_mmu_pte_prot(true);
	int ret;

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size) || size == 0)
		return -EINVAL;

	if (!is_mmio_range(phys, size))
		return -EINVAL;

	/* The vaddr == phys for the host MMU */
	ret = check_page_owner_and_state(&host_mmu, phys, size, PKVM_ID_HYP, PKVM_PAGE_NONE);
	if (ret)
		return ret;

	return pkvm_pgtable_map(&host_mmu, phys, phys, size, prot, NULL);
}

/**
 * pkvm_host_share_hyp() - Shares host pages with the pKVM hypervisor.
 * @phys:	Physical address of the memory region to share.
 * @size:	Size of the memory region to share.
 *
 * The sharing transfers the memory pages in range [@phys, @phys + @size) which
 * are exclusively owned by the host to shared-owned by the host and the pKVM
 * hypervisor. The host_share_hyp_count counters of those memory pages are
 * incremented. If a memory page is already shared-owned, page state will not be
 * changed but only the counter will be incremented. @phys and @size are not
 * required to be PAGE_SIZE aligned (@size is required to be non-zeroed value)
 * to support sharing arbitrary unaligned memory regions.
 *
 * Returns: 0 on success, or a negative error code on failure.
 */
int pkvm_host_share_hyp(unsigned long phys, unsigned long size)
{
	int ret;

	if (size == 0 || !is_memory_range(phys, size))
		return -EINVAL;

	pkvm_host_mmu_lock();

	for_each_pkvm_page(page, phys, size) {
		switch (page->host_state) {
		case PKVM_PAGE_OWNED:
			BUG_ON(page->host_share_hyp_count);
			continue;
		case PKVM_PAGE_SHARED_OWNED:
			if (page->host_share_hyp_count == U16_MAX) {
				ret = -ENOSPC;
				goto unlock;
			}

			/*
			 * Allow sharing an already shared page as long as it is
			 * shared with the hypervisor, not with a guest.
			 */
			if (page->host_share_hyp_count)
				continue;

			fallthrough;
		default:
			ret = -EPERM;
			goto unlock;
		}
	}

	for_each_pkvm_page(page, phys, size) {
		page->host_state = PKVM_PAGE_SHARED_OWNED;
		page->host_share_hyp_count++;
	}

	ret = 0;

unlock:
	pkvm_host_mmu_unlock();

	return ret;
}

/**
 * pkvm_host_unshare_hyp() - Un-share host pages with the pKVM hypervisor.
 * @phys:	Physical address of the memory region to unshare.
 * @size:	Size of the memory region to unshare.
 *
 * Decrements the host_share_hyp_count counter for the memory pages in range
 * [@phys, @phys + @size) which are shared-owned owned by the host and the pKVM
 * hypervisor, and if a page's host_share_hyp_count becomes zero, then changes
 * this page's state to make it exclusively owned by the host. @phys and @size
 * are not required to be PAGE_SIZE aligned (@size is required to be non-zeroed
 * value), to support unsharing arbitrary unaligned memory regions.
 */
void pkvm_host_unshare_hyp(unsigned long phys, unsigned long size)
{
	int ret;

	if (size == 0) {
		ret = -EINVAL;
		goto out;
	}

	pkvm_host_mmu_lock();

	ret = check_host_mem_pgstate(phys, size, PKVM_PAGE_SHARED_OWNED);
	if (ret)
		goto unlock;

	for_each_pkvm_page(page, phys, size) {
		/*
		 * Even if host_share_hyp_count is 0 because the page is
		 * shared with a guest, not with the hypervisor, it still
		 * means a pKVM bug, since pkvm_host_unshare_hyp() is only
		 * used by pKVM itself, for pages that are known to be
		 * shared with the hypervisor.
		 */
		BUG_ON(!page->host_share_hyp_count);
		if (--page->host_share_hyp_count)
			continue;

		page->host_state = PKVM_PAGE_OWNED;
	}
unlock:
	pkvm_host_mmu_unlock();
out:
	/*
	 * pkvm_host_unshare_hyp() is only used by the pKVM hypervisor itself,
	 * not on the host behalf, so it is supposed to be called with correct
	 * parameters, and only for pages that are known to be shared with the
	 * hypervisor. So any error here means a pKVM bug.
	 */
	BUG_ON(ret);
}
