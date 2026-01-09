/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2026 Google
 */

#ifndef _PKVM_INTEL_IOMMU_H_
#define _PKVM_INTEL_IOMMU_H_

#ifndef __PKVM_HYP__
int __init pkvm_host_prepare_iommu(void);
int __init pkvm_host_init_iommu(void);
#else
int pkvm_intel_iommu_init(void);
#endif
#endif
