/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2026 Google
 */

#ifndef _PKVM_INTEL_IOMMU_H_
#define _PKVM_INTEL_IOMMU_H_

/* expose pkvm_enabled() when !CONFIG_PKVM_X86 */
#include <asm/kvm_host.h>
#include <asm/kvm_pkvm.h>

/* Page table level represented by IOMMU cap SAGAW bits */
#define IOMMU_PGT_4LEVEL	BIT(2)
#define IOMMU_PGT_5LEVEL	BIT(3)

struct intel_iommu_info {
	u64 reg_phys;
	u64 reg_size;
	u64 cap;
	u64 ecap;
	int seq_id;
	int agaw;
	int msagaw;
};

#ifdef CONFIG_PKVM_INTEL
extern unsigned int pkvm_sym(iommu_pglvl_mask);
extern unsigned int pkvm_sym(iommu_pgsz_mask);

extern int pkvm_sym(intel_iommu_sm);

PKVM_DECLARE(int, prepare_iommu, (struct intel_iommu_info *info));

#ifndef __PKVM_HYP__
int __init pkvm_host_prepare_iommu(void);
int __init pkvm_host_init_iommu(void);
#else /* __PKVM_HYP__ */
static inline bool iommu_supports_2m_page(void)
{
	return iommu_pgsz_mask & (1 << PG_LEVEL_2M);
}

static inline bool iommu_supports_1g_page(void)
{
	return iommu_pgsz_mask & (1 << PG_LEVEL_1G);
}

static inline bool iommu_supports_5levels(void)
{
	return iommu_pglvl_mask & IOMMU_PGT_5LEVEL;
}

int pkvm_intel_iommu_init(void);

int pkvm_iommu_mmio_read(u64 phys, int len, u64 *val);
int pkvm_iommu_mmio_write(u64 phys, int len, u64 val);
#endif /* !__PKVM_HYP__ */
#endif /* CONFIG_PKVM_INTEL */
#endif /* _PKVM_INTEL_IOMMU_H_ */
