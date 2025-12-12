/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IO_PGTABLE_ARM_H_
#define IO_PGTABLE_ARM_H_

#include <linux/io-pgtable.h>

#define ARM_LPAE_TCR_TG0_4K		0
#define ARM_LPAE_TCR_TG0_64K		1
#define ARM_LPAE_TCR_TG0_16K		2

#define ARM_LPAE_TCR_TG1_16K		1
#define ARM_LPAE_TCR_TG1_4K		2
#define ARM_LPAE_TCR_TG1_64K		3

#define ARM_LPAE_TCR_SH_NS		0
#define ARM_LPAE_TCR_SH_OS		2
#define ARM_LPAE_TCR_SH_IS		3

#define ARM_LPAE_TCR_RGN_NC		0
#define ARM_LPAE_TCR_RGN_WBWA		1
#define ARM_LPAE_TCR_RGN_WT		2
#define ARM_LPAE_TCR_RGN_WB		3

#define ARM_LPAE_TCR_PS_32_BIT		0x0ULL
#define ARM_LPAE_TCR_PS_36_BIT		0x1ULL
#define ARM_LPAE_TCR_PS_40_BIT		0x2ULL
#define ARM_LPAE_TCR_PS_42_BIT		0x3ULL
#define ARM_LPAE_TCR_PS_44_BIT		0x4ULL
#define ARM_LPAE_TCR_PS_48_BIT		0x5ULL
#define ARM_LPAE_TCR_PS_52_BIT		0x6ULL

typedef u64 arm_lpae_iopte;

void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
			 struct io_pgtable_cfg *cfg);
void __arm_lpae_free_pages(void *pages, size_t size,
			   struct io_pgtable_cfg *cfg,
			   void *cookie);
void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
			     struct io_pgtable_cfg *cfg,
			     void *cookie);
void *__arm_lpae_alloc_data(size_t size, gfp_t gfp);
void __arm_lpae_free_data(void *p);
#ifndef __KVM_NVHE_HYPERVISOR__
#define __arm_lpae_virt_to_phys	__pa
#define __arm_lpae_phys_to_virt	__va
#endif /* !__KVM_NVHE_HYPERVISOR__ */

#endif /* IO_PGTABLE_ARM_H_ */
