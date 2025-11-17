// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/auxiliary_bus.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "arm-smmu-v3.h"
#include "pkvm/arm_smmu_v3.h"

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

static size_t smmu_hyp_pgt_pages(void)
{
	/*
	 * SMMUv3 uses the same format as stage-2 and hence have the same memory
	 * requirements, we add extra 500 pages for L2 ste.
	 */
	if (of_find_compatible_node(NULL, NULL, "arm,smmu-v3"))
		return host_s2_pgtable_pages() + 500;
	return 0;
}

static struct platform_driver smmuv3_nesting_driver;
static int smmuv3_nesting_probe(struct platform_device *pdev)
{
	return 0;
}

static int kvm_arm_smmu_v3_register(void)
{
	size_t nr_pages = smmu_hyp_pgt_pages();
	int ret;

	if (!is_protected_kvm_enabled() || !nr_pages)
		return 0;

	ret = platform_driver_probe(&smmuv3_nesting_driver, smmuv3_nesting_probe);
	if (ret)
		return ret;

	return kvm_iommu_register_driver(kern_hyp_va(lm_alias(&kvm_nvhe_sym(smmu_ops))),
					 nr_pages);
};

static int smmu_create_aux_device(struct device *dev, void *data)
{
	static int dev_id;
	struct auxiliary_device *auxdev;

	auxdev = __devm_auxiliary_device_create(dev, "protected_kvm",
						"smmu_v3_emu", NULL, dev_id++);
	if (!auxdev)
		return -ENODEV;

	auxdev->dev.parent = dev;

	return 0;
}

static struct platform_driver smmuv3_nesting_driver;
static int kvm_arm_smmu_v3_post_init(void)
{
	if (!is_protected_kvm_enabled())
		return 0;

	WARN_ON(driver_for_each_device(&smmuv3_nesting_driver.driver, NULL,
				       NULL, smmu_create_aux_device));

	return 0;
}

static const struct of_device_id smmuv3_nested_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};

static struct platform_driver smmuv3_nesting_driver = {
	.driver = {
		.name = "smmuv3-nesting",
		.of_match_table = smmuv3_nested_of_match,
	},
};
late_initcall(kvm_arm_smmu_v3_post_init);
subsys_initcall(kvm_arm_smmu_v3_register);
