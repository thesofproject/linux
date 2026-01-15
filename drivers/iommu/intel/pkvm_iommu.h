/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2026 Google
 */

#ifndef _PKVM_INTEL_IOMMU_H_
#define _PKVM_INTEL_IOMMU_H_

/* expose pkvm_enabled() when !CONFIG_PKVM_X86 */
#include <asm/kvm_host.h>
#include <asm/kvm_pkvm.h>

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
extern int pkvm_sym(intel_iommu_sm);

PKVM_DECLARE(int, prepare_iommu, (struct intel_iommu_info *info));

#ifndef __PKVM_HYP__
int __init pkvm_host_prepare_iommu(void);
int __init pkvm_host_init_iommu(void);
#else /* __PKVM_HYP__ */
int pkvm_intel_iommu_init(void);
#endif /* !__PKVM_HYP__ */
#endif /* CONFIG_PKVM_INTEL */
#endif /* _PKVM_INTEL_IOMMU_H_ */
