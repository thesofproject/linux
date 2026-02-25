// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2026 Google. */


#include <linux/hashtable.h>
#include <asm/pkvm_spinlock.h>
#include "pkvm/debug.h"
#include "pkvm/memory.h"
#include "../iommu.h"

/*
 * TODO: Make this a dynamic value.
 */
#define MAX_IOMMU_DOMAIN_NUM	128
static DEFINE_HASHTABLE(iommu_domain_hasht, 8);
static DECLARE_BITMAP(iommu_domains_bitmap, MAX_IOMMU_DOMAIN_NUM);
static struct dmar_domain iommu_domains[MAX_IOMMU_DOMAIN_NUM];
static DEFINE_PKVM_SPINLOCK(iommu_domain_lock);
struct dmar_domain pt_domain;

void init_pt_domain(void)
{
	INIT_LIST_HEAD(&pt_domain.cache_tags);
	pkvm_spin_lock_init(&pt_domain.cache_lock);
	pt_domain.qi_batch = &pt_domain._qi_batch;
}

static struct dmar_domain *__pkvm_get_iommu_domain_locked(void *pgd, bool inc_ref)
{
	struct dmar_domain *domain;

	hash_for_each_possible(iommu_domain_hasht, domain, hnode, (u64)pgd) {
		if (domain->pgd != pgd)
			continue;

		if (inc_ref && WARN_ON_ONCE(!atomic_inc_not_zero(&domain->refcount)))
			return NULL;

		return domain;
	}

	return NULL;
}

struct dmar_domain *pkvm_get_iommu_domain(void *pgd)
{
	struct dmar_domain *domain;

	pkvm_spin_lock(&iommu_domain_lock);
	domain = __pkvm_get_iommu_domain_locked(pgd, true);
	pkvm_spin_unlock(&iommu_domain_lock);

	return domain;
}

/*
 * Retrieve the domain without incrementing refcount.
 * This api is useful when there is a refcount on the domain
 * and refcount is guaranteed to be not dropped.
 */
struct dmar_domain *pkvm_get_iommu_domain_noref(void *pgd)
{
	struct dmar_domain *domain;

	pkvm_spin_lock(&iommu_domain_lock);
	domain = __pkvm_get_iommu_domain_locked(pgd, false);
	pkvm_spin_unlock(&iommu_domain_lock);

	return domain;
}

void pkvm_put_iommu_domain(struct dmar_domain *domain)
{
	WARN_ON_ONCE(atomic_dec_if_positive(&domain->refcount) <= 0);
}

int pkvm_get_domain_cache_tag_assign(void *pgd, int did, u32 pasid,
				     struct device_domain_info *info)
{
	struct pkvm_device dev = { .info = info };
	struct dmar_domain *domain;
	int ret;

	if (did == FLPT_DEFAULT_DID) {
		cache_tag_assign_domain(&pt_domain, did, &dev, pasid);
		return 0;
	}

	domain = pkvm_get_iommu_domain(pgd);
	if (!domain) {
		pkvm_err("%s: Failed to locate domain with pgd: %p\n",
			 __func__, pgd);
		return -EFAULT;
	}

	ret = cache_tag_assign_domain(domain, did, &dev, pasid);
	if (ret) {
		pkvm_put_iommu_domain(domain);
		return ret;
	}
	return 0;
}

void pkvm_put_domain_cache_tag_unassign(void *pgd, int did, u32 pasid,
					struct device_domain_info *info)
{
	struct pkvm_device dev = { .info = info };
	struct dmar_domain *domain;

	if (did == FLPT_DEFAULT_DID) {
		cache_tag_unassign_domain(&pt_domain, did, &dev, pasid);
		return;
	}

	domain = pkvm_get_iommu_domain_noref(pgd);
	BUG_ON(!domain);

	cache_tag_unassign_domain(domain, did, &dev, pasid);
	pkvm_put_iommu_domain(domain);
}

int pkvm_free_iommu_domain(struct dmar_domain *domain)
{
	if (atomic_cmpxchg(&domain->refcount, 1, 0) != 1) {
		pkvm_err("%s: domain[pgd:%p] has users, refcount %d\n",
			 __func__, domain->pgd, atomic_read(&domain->refcount));
		return -EBUSY;
	}

	pkvm_dbg("%s: freed domain pgd: %p\n", __func__, domain->pgd);
	pkvm_spin_lock(&iommu_domain_lock);
	hash_del(&domain->hnode);
	__clear_bit(domain->index, iommu_domains_bitmap);
	memset(domain, 0, sizeof(struct dmar_domain));
	pkvm_spin_unlock(&iommu_domain_lock);

	return 0;
}

struct dmar_domain *pkvm_alloc_iommu_domain(struct alloc_domain_data *data)
{
	void *pgd = pkvm_host_gpa_to_virt(data->pgd_gpa);
	struct dmar_domain *domain;
	unsigned long index;

	pkvm_spin_lock(&iommu_domain_lock);
	domain = __pkvm_get_iommu_domain_locked(pgd, false);
	if (unlikely(domain)) {
		pkvm_spin_unlock(&iommu_domain_lock);
		return ERR_PTR(-EEXIST);
	}

	index = find_first_zero_bit(iommu_domains_bitmap, MAX_IOMMU_DOMAIN_NUM);
	if (index < MAX_IOMMU_DOMAIN_NUM) {
		__set_bit(index, iommu_domains_bitmap);
		domain = &iommu_domains[index];
		INIT_LIST_HEAD(&domain->cache_tags);
		domain->pgd = pgd;
		domain->use_first_level = data->use_first_level;
		domain->iommu_superpage = data->iommu_superpage;
		domain->iommu_coherency = data->iommu_coherency;
		domain->agaw = data->agaw;
		domain->gaw = data->gaw;
		domain->max_addr = data->max_addr;
		domain->index = index;
		domain->qi_batch = &domain->_qi_batch;
		atomic_set(&domain->refcount, 1);
		pkvm_spin_lock_init(&domain->lock);
		pkvm_spin_lock_init(&domain->cache_lock);
		hash_add(iommu_domain_hasht, &domain->hnode, (u64)pgd);
		pkvm_dbg("%s: allocated domain pgd: %p\n", __func__, pgd);
	} else {
		domain = ERR_PTR(-ENOMEM);
	}
	pkvm_spin_unlock(&iommu_domain_lock);

	return domain;
}
