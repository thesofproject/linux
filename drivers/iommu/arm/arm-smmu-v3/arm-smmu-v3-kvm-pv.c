// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "pkvm/arm_smmu_v3.h"
#include "arm-smmu-v3.h"

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_pv_ops);

#ifdef MODULE
#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x)) *)(pkvm_el2_mod_va(&kvm_nvhe_sym(x))))

int kvm_nvhe_sym(smmu_init_hyp_module)(const struct pkvm_module_ops *ops);
#else
#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x)) *)(kern_hyp_va(lm_alias(&kvm_nvhe_sym(x)))))
#endif

static size_t smmu_hyp_pgt_pages(void)
{
	/*
	 * SMMUv3 uses the same format as stage-2 and hence have the same memory
	 * requirements, we add extra 500 pages for L2 ste.
	 * For modules, we can't use host_s2_pgtable_pages(), so we return 1 page,
	 * and rely on the vendor passing the right commandline arg.
	 */
	if (of_find_compatible_node(NULL, NULL, "arm,smmu-v3")) {
#ifdef MODULE
		return 1;
#else
		return host_s2_pgtable_pages() + 500;
#endif
	}
	return 0;
}

static int kvm_arm_smmu_probe(struct platform_device *pdev)
{
	return -ENOSYS;
}

static void kvm_arm_smmu_remove(struct platform_device *pdev)
{
}

static const struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};

static struct platform_driver kvm_arm_smmu_driver = {
	.driver = {
		.name = "kvm-arm-smmu-v3",
		.of_match_table = arm_smmu_of_match,
	},
	.remove = kvm_arm_smmu_remove,
};

static int kvm_arm_smmu_v3_init_drv(void)
{
	int ret;

	ret = platform_driver_probe(&kvm_arm_smmu_driver, kvm_arm_smmu_probe);
	if (ret)
		return ret;

#ifdef MODULE
	ret = pkvm_load_el2_module(kvm_nvhe_sym(smmu_init_hyp_module));
	if (ret) {
		pr_err("Failed to load SMMUv3 IOMMU EL2 module: %d\n", ret);
		return ret;
	}
#endif

	ret = kvm_iommu_register_hyp_ops(ksym_ref_addr_nvhe(smmu_pv_ops));
	if (ret)
		return ret;

	return 0;
}

static struct kvm_iommu_driver kvm_smmu_v3_ops = {
	.init_driver = kvm_arm_smmu_v3_init_drv,
};

static int kvm_arm_smmu_v3_register(void)
{
	if (!is_protected_kvm_enabled())
		return 0;

	return kvm_iommu_register_driver(&kvm_smmu_v3_ops, smmu_hyp_pgt_pages());
};

/*
 * Register must be run before de-privliage before kvm_iommu_init_driver
 * for module case, it should be loaded using pKVM early loading which
 * loads it before this point.
 * For builtin drivers we use core_initcall
 */
#ifdef MODULE
module_init(kvm_arm_smmu_v3_register);
#else
core_initcall(kvm_arm_smmu_v3_register);
#endif
MODULE_LICENSE("GPL v2");
