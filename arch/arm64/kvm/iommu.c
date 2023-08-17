// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <linux/kvm_host.h>

extern size_t kvm_nvhe_sym(hyp_kvm_iommu_pages);
static struct kvm_iommu_driver *iommu_driver;

int kvm_iommu_register_driver(struct kvm_iommu_driver *kern_ops, size_t pool_pages)
{
	if (!kern_ops)
		return -ENODEV;

	/* See kvm_iommu_pages() */
	if (pool_pages > kvm_nvhe_sym(hyp_kvm_iommu_pages)) {
		kvm_err("Missing memory for the IOMMU pool, need 0x%zx pages, check kvm-arm.hyp_iommu_pages",
			 pool_pages);
		return -ENOMEM;
	}

	/*
	 * Paired with smp_load_acquire(&iommu_driver)
	 * Ensure memory stores happening during a driver
	 * init are observed before executing kvm iommu callbacks.
	 */
	return cmpxchg_release(&iommu_driver, NULL, kern_ops) ? -EBUSY : 0;
}
EXPORT_SYMBOL(kvm_iommu_register_driver);

int kvm_iommu_register_hyp_ops(struct kvm_iommu_ops *hyp_ops)
{
	if (!hyp_ops)
		return -ENODEV;

	return kvm_call_hyp_nvhe(__pkvm_iommu_register_ops, hyp_ops);
}
EXPORT_SYMBOL(kvm_iommu_register_hyp_ops);

int kvm_iommu_init_driver(void)
{
	/* See kvm_iommu_register_driver() */
	if (!smp_load_acquire(&iommu_driver)) {
		kvm_err("pKVM enabled without an IOMMU driver, do not run confidential workloads in virtual machines\n");
		return -ENODEV;
	}

	if (iommu_driver->init_driver)
		return iommu_driver->init_driver();

	return 0;
}

size_t kvm_iommu_pages(void)
{
	/*
	 * This is called very early during setup_arch() where no initcalls,
	 * so this has to call specific functions per each KVM driver.
	 * So we allow a config option that can set the defaul value for
	 * the IOMMU pool that can overridden by a command line option.
	 * When the driver registers it will pass the number pages needed
	 * for it's page tables, if less that what the system has already
	 * allocated we fail.
	 */
	return kvm_nvhe_sym(hyp_kvm_iommu_pages);
}

/* Number of pages to reserve for iommu pool*/
static int __init early_hyp_iommu_pages(char *arg)
{
	return kstrtoul(arg, 10, &kvm_nvhe_sym(hyp_kvm_iommu_pages));
}
early_param("kvm-arm.hyp_iommu_pages", early_hyp_iommu_pages);
