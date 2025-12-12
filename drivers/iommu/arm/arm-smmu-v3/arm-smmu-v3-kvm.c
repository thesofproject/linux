// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/auxiliary_bus.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "arm-smmu-v3.h"
#include "pkvm/arm_smmu_v3.h"

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device	*kvm_arm_smmu_array;
static size_t				kvm_arm_smmu_cur;

static void kvm_arm_smmu_array_free(void)
{
	int order;

	order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	free_pages((unsigned long)kvm_arm_smmu_array, order);
}

/*
 * The hypervisor have to know the basic information about the SMMUs
 * from the firmware.
 * This has to be done before the SMMUv3 probes and does anything meaningful
 * with the hardware, otherwise it becomes harder to reason about the SMMU
 * state and we'd require to hand-off the state to the hypervisor at certain point
 * while devices are live, which is complicated and dangerous.
 * Instead, the hypervisor is interested in a very small part of the probe path,
 * so just add a separate logic for it.
 */
static int kvm_arm_smmu_array_alloc(void)
{
	int smmu_order;
	struct device_node *np;

	for_each_compatible_node(np, NULL, "arm,smmu-v3")
		kvm_arm_smmu_count++;

	if (!kvm_arm_smmu_count)
		return -ENODEV;
	smmu_order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	kvm_arm_smmu_array = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, smmu_order);
	if (!kvm_arm_smmu_array)
		return -ENOMEM;
	return 0;
}

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
	struct resource *res;
	struct hyp_arm_smmu_v3_device *smmu = &kvm_arm_smmu_array[kvm_arm_smmu_cur];

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	smmu->mmio_addr = res->start;
	smmu->mmio_size = resource_size(res);
	if (smmu->mmio_size < SZ_128K) {
		dev_err(&pdev->dev, "MMIO region too small(%pr)\n", &res);
		return -EINVAL;
	}

	if (of_dma_is_coherent(pdev->dev.of_node))
		smmu->features |= ARM_SMMU_FEAT_COHERENCY;

	kvm_arm_smmu_cur++;
	return 0;
}

static int kvm_arm_smmu_v3_register(void)
{
	size_t nr_pages = smmu_hyp_pgt_pages();
	int ret;

	if (!is_protected_kvm_enabled() || !nr_pages)
		return 0;

	ret = kvm_arm_smmu_array_alloc();
	if (ret)
		return ret;

	ret = platform_driver_probe(&smmuv3_nesting_driver, smmuv3_nesting_probe);
	if (ret)
		goto out_err;

	ret = kvm_iommu_register_driver(kern_hyp_va(lm_alias(&kvm_nvhe_sym(smmu_ops))),
					nr_pages);
	if (ret)
		goto out_err;

	/*
	 * These variables are stored in the nVHE image, and won't be accessible
	 * after KVM initialization. Ownership of kvm_arm_smmu_array will be
	 * transferred to the hypervisor as well.
	 */
	kvm_hyp_arm_smmu_v3_smmus = kvm_arm_smmu_array;
	kvm_hyp_arm_smmu_v3_count = kvm_arm_smmu_count;
	return ret;

out_err:
	kvm_arm_smmu_array_free();
	return ret;
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
	if (!is_protected_kvm_enabled() || !kvm_arm_smmu_cur)
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
