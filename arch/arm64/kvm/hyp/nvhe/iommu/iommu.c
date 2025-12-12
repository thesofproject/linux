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
#include <nvhe/mm.h>
#include <nvhe/spinlock.h>

/* Only one set of ops supported */
struct kvm_iommu_ops *kvm_iommu_ops;

/* Protected by host_mmu.lock */
static bool kvm_idmap_initialized;
static struct hyp_pool iommu_pages_pool_atomic;
static struct hyp_pool iommu_host_pool;

DECLARE_PER_CPU(struct kvm_hyp_req, host_hyp_reqs);

static struct kvm_hyp_iommu_domain kvm_iommu_domains[KVM_IOMMU_MAX_DOMAINS];

/* Protects domains in kvm_iommu_domains */
static DEFINE_HYP_SPINLOCK(kvm_iommu_domain_lock);

static int kvm_iommu_refill(struct kvm_hyp_memcache *host_mc)
{
	if (!kvm_iommu_ops)
		return -EINVAL;

	return refill_hyp_pool(&iommu_host_pool, host_mc);
}

static void kvm_iommu_reclaim(struct kvm_hyp_memcache *host_mc, int target)
{
	if (!kvm_iommu_ops)
		return;

	reclaim_hyp_pool(&iommu_host_pool, host_mc, target);
}

static int kvm_iommu_reclaimable(void)
{
	if (!kvm_iommu_ops)
		return 0;

	return hyp_pool_free_pages(&iommu_host_pool);
}

struct hyp_mgt_allocator_ops kvm_iommu_allocator_ops = {
	.refill = kvm_iommu_refill,
	.reclaim = kvm_iommu_reclaim,
	.reclaimable = kvm_iommu_reclaimable,
};

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
	struct kvm_iommu_ops *ops = (struct kvm_iommu_ops *)ctx->arg;

	/* Keep unmapped. */
	if (pte && !kvm_pte_valid(pte))
		return 0;

	if (kvm_pte_valid(pte))
		prot = pkvm_to_iommu_prot(kvm_pgtable_stage2_pte_prot(pte));
	else if (!addr_is_memory(start))
		prot |= IOMMU_MMIO;

	ops->host_stage2_idmap(start, end, prot);
	return 0;
}

static int kvm_iommu_snapshot_host_stage2(struct kvm_iommu_ops *ops)
{
	int ret;
	struct kvm_pgtable_walker walker = {
		.cb	= __snapshot_host_stage2,
		.flags	= KVM_PGTABLE_WALK_LEAF,
		.arg = ops,
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
	if (nr_pages) {
		int ret;

		ret = hyp_pool_init(&iommu_pages_pool_atomic, hyp_virt_to_pfn(pool_base),
				    nr_pages, 0);
		if (ret)
			return ret;
	}

	return hyp_pool_init_empty(&iommu_host_pool, 64);
}

int kvm_iommu_register_ops(struct kvm_iommu_ops *ops)
{
	int ret;

	if (!ops || !ops->init ||
	    !ops->host_stage2_idmap)
		return -ENODEV;

	ret = ops->init();
	if (ret)
		return ret;

	ret = kvm_iommu_snapshot_host_stage2(ops);
	if (ret)
		return ret;

	kvm_iommu_ops = ops;
	return 0;
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

void *kvm_iommu_donate_pages(u8 order, int flags)
{
	void *p;
	struct kvm_hyp_req *req = this_cpu_ptr(&host_hyp_reqs);
	size_t size = (1 << order) * PAGE_SIZE;

	p = hyp_alloc_pages(&iommu_host_pool, order);
	if (p)
		return p;

	req->type = KVM_HYP_REQ_TYPE_MEM;
	req->mem.dest = REQ_MEM_DEST_HYP_IOMMU;
	req->mem.sz_alloc = size;
	req->mem.nr_pages = 1;
	return NULL;
}

void kvm_iommu_reclaim_pages(void *p, u8 order)
{
	hyp_put_page(&iommu_host_pool, p);
}

void *kvm_iommu_donate_pages_atomic(u8 order)
{
	return hyp_alloc_pages(&iommu_pages_pool_atomic, order);
}

void kvm_iommu_reclaim_pages_atomic(void *ptr)
{
	hyp_put_page(&iommu_pages_pool_atomic, ptr);
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

static struct kvm_hyp_iommu_domain *handle_to_domain(pkvm_handle_t domain_id)
{
	if (domain_id >= KVM_IOMMU_MAX_DOMAINS)
		return NULL;

	domain_id = array_index_nospec(domain_id, KVM_IOMMU_MAX_DOMAINS);

	return &kvm_iommu_domains[domain_id];
}

static int domain_get(struct kvm_hyp_iommu_domain *domain)
{
	int old = atomic_fetch_inc_acquire(&domain->refs);

	BUG_ON(!old || (old + 1 < 0));
	return 0;
}

static void domain_put(struct kvm_hyp_iommu_domain *domain)
{
	BUG_ON(!atomic_dec_return_release(&domain->refs));
}

int kvm_iommu_alloc_domain(pkvm_handle_t iommu_id, pkvm_handle_t domain_id, int type)
{
	int ret = -EINVAL;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->alloc_domain)
		return -ENODEV;

	domain = handle_to_domain(domain_id);
	if (!domain)
		return -ENOMEM;

	hyp_spin_lock(&kvm_iommu_domain_lock);
	if (atomic_read(&domain->refs))
		goto out_unlock;

	domain->domain_id = domain_id;
	ret = kvm_iommu_ops->alloc_domain(iommu_id, domain, type);
	if (ret)
		goto out_unlock;

	atomic_set_release(&domain->refs, 1);
out_unlock:
	hyp_spin_unlock(&kvm_iommu_domain_lock);
	return ret;
}

int kvm_iommu_free_domain(pkvm_handle_t domain_id)
{
	int ret = 0;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->free_domain)
		return -ENODEV;

	domain = handle_to_domain(domain_id);
	if (!domain)
		return -EINVAL;

	hyp_spin_lock(&kvm_iommu_domain_lock);
	if (WARN_ON(atomic_cmpxchg_acquire(&domain->refs, 1, 0) != 1)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	kvm_iommu_ops->free_domain(domain);
	memset(domain, 0, sizeof(*domain));
out_unlock:
	hyp_spin_unlock(&kvm_iommu_domain_lock);

	return ret;
}

int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid, u32 pasid_bits, unsigned long flags)
{
	int ret;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->attach_dev)
		return -ENODEV;

	hyp_spin_lock(&kvm_iommu_domain_lock);
	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = kvm_iommu_ops->attach_dev(iommu_id, domain,
				        endpoint_id, pasid, pasid_bits, flags);
	if (ret)
		domain_put(domain);
out_unlock:
	hyp_spin_unlock(&kvm_iommu_domain_lock);
	return ret;
}

int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid)
{
	int ret;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->detach_dev)
		return -ENODEV;

	hyp_spin_lock(&kvm_iommu_domain_lock);
	domain = handle_to_domain(domain_id);
	if (!domain || atomic_read(&domain->refs) <= 1) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = kvm_iommu_ops->detach_dev(iommu_id, domain, endpoint_id, pasid);
	if (ret)
		goto out_unlock;
	domain_put(domain);
out_unlock:
	hyp_spin_unlock(&kvm_iommu_domain_lock);
	return ret;
}

#define IOMMU_PROT_MASK (IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE |\
			 IOMMU_NOEXEC | IOMMU_MMIO | IOMMU_PRIV)

int kvm_iommu_map_pages(pkvm_handle_t domain_id,
			unsigned long iova, phys_addr_t paddr, size_t pgsize,
			size_t pgcount, int prot, unsigned long *mapped)
{
	size_t size;
	int ret;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->map_pages)
		return -ENODEV;

	*mapped = 0;

	if (prot & ~IOMMU_PROT_MASK)
		return -EOPNOTSUPP;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova || paddr + size < paddr)
		return -E2BIG;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return -ENOENT;

	ret = kvm_iommu_ops->map_pages(domain, iova, paddr, pgsize, pgcount,
				       prot, mapped);

	domain_put(domain);
	/* Mask -ENOMEM, as it's passed as a request. */
	return ret == -ENOMEM ? 0 : ret;
}

size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id, unsigned long iova,
			     size_t pgsize, size_t pgcount)
{
	size_t size;
	size_t unmapped;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->unmap_pages)
		return -ENODEV;

	if (!pgsize || !pgcount)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova)
		return 0;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return 0;

	unmapped = kvm_iommu_ops->unmap_pages(domain, iova, pgsize, pgcount);

	domain_put(domain);
	return unmapped;
}

phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova)
{
	phys_addr_t phys = 0;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->iova_to_phys)
		return -ENODEV;

	domain = handle_to_domain( domain_id);

	if (!domain || domain_get(domain))
		return 0;

	phys = kvm_iommu_ops->iova_to_phys(domain, iova);
	domain_put(domain);
	return phys;
}
