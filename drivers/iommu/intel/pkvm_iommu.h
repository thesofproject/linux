/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2026 Google
 */

#ifndef _PKVM_INTEL_IOMMU_H_
#define _PKVM_INTEL_IOMMU_H_

#ifdef __PKVM_HYP__
int pkvm_intel_iommu_init(void);
#endif
#endif
