/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM_SMMU_V3_MODULE__
#define __ARM_SMMU_V3_MODULE__

#if defined(__KVM_NVHE_HYPERVISOR__) && defined(MODULE)

#include <asm/kvm_pkvm_module.h>

extern const struct pkvm_module_ops		*mod_ops;

#define CALL_FROM_OPS(fn, ...)			mod_ops->fn(__VA_ARGS__)

#undef memset
#undef memcpy
#undef kvm_flush_dcache_to_poc
#undef kern_hyp_va

/* Needs alternatives which is not supported at the moment. */
#undef CONFIG_ARM64_LSE_ATOMICS

#define kvm_iommu_donate_pages_atomic(x)		CALL_FROM_OPS(iommu_donate_pages_atomic, x)
#define kvm_iommu_reclaim_pages_atomic(x)		CALL_FROM_OPS(iommu_reclaim_pages_atomic, x)
#define hyp_virt_to_phys(x)			CALL_FROM_OPS(hyp_pa, x)
#define hyp_phys_to_virt(x)			CALL_FROM_OPS(hyp_va, x)
#define memcpy(x, y, z)				CALL_FROM_OPS(memcpy, x, y, z)
#define pkvm_time_get(x)			CALL_FROM_OPS(get_time, x)
#define kvm_flush_dcache_to_poc(x, y)		CALL_FROM_OPS(flush_dcache_to_poc, x, y)
#define ___pkvm_host_donate_hyp_prot(x, y, z, w) CALL_FROM_OPS(host_donate_hyp_prot, x, y, z, w)
/* Only used for MMIO. */
#define ___pkvm_host_donate_hyp(x, y, z)	 CALL_FROM_OPS(host_donate_hyp_prot, x, y, z, PAGE_HYP_DEVICE)
#define kern_hyp_va(x)				(void *)CALL_FROM_OPS(kern_hyp_va, (unsigned long)x)
#define __pkvm_host_donate_hyp(x, y)		CALL_FROM_OPS(host_donate_hyp, x, y)
#define __pkvm_hyp_donate_host(x, y)		CALL_FROM_OPS(hyp_donate_host, x, y)
#define __pkvm_host_share_hyp(x)		CALL_FROM_OPS(host_share_hyp, x)
#define hyp_pin_shared_mem(x, y)		CALL_FROM_OPS(pin_shared_mem, x, y)
#define __pkvm_host_unshare_hyp(x)		CALL_FROM_OPS(host_unshare_hyp, x)
#define hyp_unpin_shared_mem(x, y)		CALL_FROM_OPS(unpin_shared_mem, x, y)
#define kvm_iommu_donate_pages(x, y)		CALL_FROM_OPS(iommu_donate_pages, x, y)
#define kvm_iommu_reclaim_pages(x, y)		CALL_FROM_OPS(iommu_reclaim_pages, x, y)

#endif


#endif /* __ARM_SMMU_V3_MODULE__ */
