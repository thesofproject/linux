// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2026, Intel Corporation.
 *
 * Author: Prabu Renganathan <prabu.renganathan@intel.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/iova.h>
#include <linux/bits.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sizes.h>
#include <trace/hooks/iommu.h>

static unsigned int align_shift = 9;
module_param(align_shift, uint, 0444);
MODULE_PARM_DESC(align_shift, "Max IOVA alignment shift (log2 pages); default=9 (2MB @ 4K pages)");

static void iova_limit_align_shift_handler(void *data,
					   struct iova_domain *iovad,
					   unsigned long size,
					   unsigned long *shift)
{
	if (*shift > align_shift)
		*shift = align_shift;
}

static int __init iova_limit_align_shift_init(void)
{
	int ret;

	if (align_shift >= BITS_PER_LONG) {
		pr_err("invalid align_shift=%u (must be < %u)\n",
		       align_shift, BITS_PER_LONG);
		return -EINVAL;
	}

	ret = register_trace_android_rvh_iommu_limit_align_shift(
			iova_limit_align_shift_handler, NULL);
	if (ret) {
		pr_err("failed to register alignment hook: %d\n", ret);
		return ret;
	}

	pr_info("IOVA alignment shift capped at %u (%lu pages, %luMB)\n",
		align_shift,
		1UL << align_shift,
		(1UL << align_shift) * PAGE_SIZE / SZ_1M);

	return 0;
}

static int __init vendor_hooks_init(void)
{
	int ret;

	ret = iova_limit_align_shift_init();
	if (ret)
		pr_warn("iova_limit_align_shift_init failed: %d\n", ret);

	return 0;
}

module_init(vendor_hooks_init);

MODULE_AUTHOR("Prabu Renganathan <prabu.renganathan@intel.com>");
MODULE_DESCRIPTION("Intel Vendor Hooks");
MODULE_LICENSE("GPL");
