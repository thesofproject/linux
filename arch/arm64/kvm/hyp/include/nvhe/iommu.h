/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

#include <asm/kvm_host.h>
#include <asm/kvm_pgtable.h>

#include <nvhe/alloc_mgt.h>

struct kvm_hyp_iommu_domain {
	atomic_t		refs;
	pkvm_handle_t		domain_id;
	void			*priv;
};

struct kvm_iommu_ops {
	int (*init)(void);
	void (*host_stage2_idmap)(phys_addr_t start, phys_addr_t end, int prot);
	bool (*dabt_handler)(struct user_pt_regs *regs, u64 esr, u64 addr);
	void (*host_stage2_idmap_complete)(bool map);
	int (*alloc_domain)(pkvm_handle_t iommu_id, struct kvm_hyp_iommu_domain *domain, int type);
	void (*free_domain)(struct kvm_hyp_iommu_domain *domain);
};

int kvm_iommu_init(void *pool_base, size_t nr_pages);
int kvm_iommu_register_ops(struct kvm_iommu_ops *ops);

void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot);
void *kvm_iommu_donate_pages_atomic(u8 order);
void kvm_iommu_reclaim_pages_atomic(void *ptr);
bool kvm_iommu_host_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr);
void kvm_iommu_host_stage2_idmap_complete(bool map);

/* Hypercall handlers */
int kvm_iommu_alloc_domain(pkvm_handle_t iommu_id, pkvm_handle_t domain_id, int type);
int kvm_iommu_free_domain(pkvm_handle_t domain_id);
int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid, u32 pasid_bits,
			 unsigned long flags);
int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid);

int kvm_iommu_map_pages(pkvm_handle_t domain_id,
			unsigned long iova, phys_addr_t paddr, size_t pgsize,
			size_t pgcount, int prot, unsigned long *mapped);
size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id, unsigned long iova,
			     size_t pgsize, size_t pgcount);
phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova);

/* Flags not used and added for future use. */
void *kvm_iommu_donate_pages(u8 order, int flags);
void kvm_iommu_reclaim_pages(void *p, u8 order);

#define kvm_iommu_donate_page()		kvm_iommu_donate_pages(0, 0)
#define kvm_iommu_reclaim_page(p)		kvm_iommu_reclaim_pages(p, 0)

extern struct hyp_mgt_allocator_ops kvm_iommu_allocator_ops;

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
