/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

#include <asm/kvm_host.h>
#include <asm/kvm_pgtable.h>

struct kvm_iommu_ops {
	int (*init)(void);
	void (*host_stage2_idmap)(phys_addr_t start, phys_addr_t end, int prot);
};

int kvm_iommu_init(void *pool_base, size_t nr_pages);

void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot);
void *kvm_iommu_donate_pages(u8 order);
void kvm_iommu_reclaim_pages(void *ptr);

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
