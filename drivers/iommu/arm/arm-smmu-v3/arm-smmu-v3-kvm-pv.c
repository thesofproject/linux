// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <asm/kvm_host.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/io-pgtable.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "pkvm/arm_smmu_v3.h"
#include "arm-smmu-v3.h"
#include "../../io-pgtable-arm.h"

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_pv_ops);

struct kvm_arm_smmu_domain {
	struct iommu_domain		domain;
	struct arm_smmu_device		*smmu;
	pkvm_handle_t			id;
};

#define to_kvm_smmu_domain(_domain) \
	container_of(_domain, struct kvm_arm_smmu_domain, domain)

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

struct host_arm_smmu_device {
	struct arm_smmu_device		smmu;
	pkvm_handle_t			id;
	u32				boot_gbpa;
	phys_addr_t			ioaddr;
};

#define smmu_to_host(_smmu) \
	container_of(_smmu, struct host_arm_smmu_device, smmu);

static size_t				kvm_arm_smmu_cur;
static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device_pv	*kvm_arm_smmu_array;
static DEFINE_IDA(kvm_arm_smmu_domain_ida);

static struct platform_driver kvm_arm_smmu_driver;

static struct arm_smmu_device *
kvm_arm_smmu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev;
	dev = driver_find_device_by_fwnode(&kvm_arm_smmu_driver.driver, fwnode);
	put_device(dev);
	return dev ? dev_get_drvdata(dev) : NULL;
}

static struct iommu_device *kvm_arm_smmu_probe_device(struct device *dev)
{
	struct arm_smmu_device *smmu;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct arm_smmu_master *master;
	int ret;
	if (WARN_ON_ONCE(dev_iommu_priv_get(dev)))
		return ERR_PTR(-EBUSY);
	smmu = kvm_arm_smmu_get_by_fwnode(fwspec->iommu_fwnode);
	if (!smmu)
		return ERR_PTR(-ENODEV);
	master = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);
	master->dev = dev;
	master->smmu = smmu;
	dev_iommu_priv_set(dev, master);
	device_property_read_u32(dev, "pasid-num-bits", &master->ssid_bits);
	master->ssid_bits = min(smmu->ssid_bits, master->ssid_bits);
	ret = arm_smmu_insert_master(smmu, master, false);
	if (ret)
		goto err_free_master;
	return &smmu->iommu;
err_free_master:
	kfree(master);
	return ERR_PTR(ret);
}

static bool kvm_arm_smmu_capable(struct device *dev, enum iommu_cap cap)
{
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return master->smmu->features & ARM_SMMU_FEAT_COHERENCY;
	case IOMMU_CAP_NOEXEC:
		return true;
	default:
		return false;
	}
}

static int kvm_arm_smmu_domain_finalize(struct kvm_arm_smmu_domain *kvm_smmu_domain,
					struct arm_smmu_master *master)
{
	int ret = 0;
	struct arm_smmu_device *smmu = master->smmu;
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);
	struct io_pgtable_cfg cfg;

	cfg.ias = smmu->ias;
	cfg.oas = smmu->oas;
	cfg.pgsize_bitmap = smmu->pgsize_bitmap;

	arm_lpae_restrict_pgsizes(&cfg);
	kvm_smmu_domain->domain.pgsize_bitmap = cfg.pgsize_bitmap;
	kvm_smmu_domain->domain.geometry.aperture_end = (1UL << cfg.ias) - 1;
	kvm_smmu_domain->domain.geometry.force_aperture = true;

	ret = ida_alloc_range(&kvm_arm_smmu_domain_ida, 1,
			      KVM_IOMMU_MAX_DOMAINS, GFP_KERNEL);
	if (ret < 0)
		return ret;

	kvm_smmu_domain->id = ret;
	ret = kvm_iommu_alloc_domain(host_smmu->id, kvm_smmu_domain->id, KVM_ARM_SMMU_DOMAIN_S1);
	if (ret) {
		ida_free(&kvm_arm_smmu_domain_ida, kvm_smmu_domain->id);
		return ret;
	}

	kvm_smmu_domain->smmu = smmu;
	return ret;
}

static struct kvm_arm_smmu_domain *kvm_arm_smmu_domain_alloc(void)
{
	struct kvm_arm_smmu_domain *smmu_domain;

	smmu_domain = kzalloc(sizeof(*smmu_domain), GFP_KERNEL);
	if (!smmu_domain)
		return ERR_PTR(-ENOMEM);
	return smmu_domain;
}

static struct iommu_domain *kvm_arm_smmu_domain_alloc_paging(struct device *dev)
{
	struct kvm_arm_smmu_domain *smmu_domain = kvm_arm_smmu_domain_alloc();
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);
	int ret;

	if (IS_ERR(smmu_domain))
		return ERR_PTR(PTR_ERR(smmu_domain));

	ret = kvm_arm_smmu_domain_finalize(smmu_domain, master);
	if (ret) {
		kfree(smmu_domain);
		return ERR_PTR(ret);
	}
	return &smmu_domain->domain;
}

static void kvm_arm_smmu_free_domain(struct iommu_domain *domain)
{
	int ret;
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	struct arm_smmu_device *smmu = kvm_smmu_domain->smmu;

	if (smmu) {
		ret = kvm_iommu_free_domain(kvm_smmu_domain->id);
		if (ret)
			dev_err(smmu->dev, "Failed to free domain %d\n", ret);
		ida_free(&kvm_arm_smmu_domain_ida, kvm_smmu_domain->id);
	}
	kfree(kvm_smmu_domain);
}

static struct iommu_ops kvm_arm_smmu_ops = {
	.capable		= kvm_arm_smmu_capable,
	.device_group		= arm_smmu_device_group,
	.of_xlate		= arm_smmu_of_xlate,
	.get_resv_regions	= arm_smmu_get_resv_regions,
	.probe_device		= kvm_arm_smmu_probe_device,
	.owner			= THIS_MODULE,
	.domain_alloc_paging	= kvm_arm_smmu_domain_alloc_paging,
	.default_domain_ops 	=  &(const struct iommu_domain_ops) {
		.free		= kvm_arm_smmu_free_domain,
	}
};

static bool kvm_arm_smmu_validate_features(struct arm_smmu_device *smmu)
{
	unsigned int required_features =
		ARM_SMMU_FEAT_TT_LE |
		ARM_SMMU_FEAT_TRANS_S2;
	unsigned int forbidden_features =
		ARM_SMMU_FEAT_STALL_FORCE;
	unsigned int keep_features =
		ARM_SMMU_FEAT_2_LVL_STRTAB	|
		ARM_SMMU_FEAT_2_LVL_CDTAB	|
		ARM_SMMU_FEAT_TT_LE		|
		ARM_SMMU_FEAT_SEV		|
		ARM_SMMU_FEAT_COHERENCY		|
		ARM_SMMU_FEAT_TRANS_S1		|
		ARM_SMMU_FEAT_TRANS_S2		|
		ARM_SMMU_FEAT_VAX		|
		ARM_SMMU_FEAT_RANGE_INV;

	if (smmu->options & ARM_SMMU_OPT_PAGE0_REGS_ONLY) {
		dev_err(smmu->dev, "unsupported layout\n");
		return false;
	}

	if ((smmu->features & required_features) != required_features) {
		dev_err(smmu->dev, "missing features 0x%x\n",
			required_features & ~smmu->features);
		return false;
	}

	if (smmu->features & forbidden_features) {
		dev_err(smmu->dev, "features 0x%x forbidden\n",
			smmu->features & forbidden_features);
		return false;
	}

	smmu->features &= keep_features;

	return true;
}

static int kvm_arm_smmu_device_reset(struct host_arm_smmu_device *host_smmu)
{
	int ret;
	u32 reg;
	struct arm_smmu_device *smmu = &host_smmu->smmu;

	reg = readl_relaxed(smmu->base + ARM_SMMU_CR0);
	if (reg & CR0_SMMUEN)
		dev_warn(smmu->dev, "SMMU currently enabled! Resetting...\n");

	/* Disable bypass */
	host_smmu->boot_gbpa = readl_relaxed(smmu->base + ARM_SMMU_GBPA);
	ret = arm_smmu_update_gbpa(smmu, GBPA_ABORT, 0);
	if (ret)
		return ret;

	ret = arm_smmu_device_disable(smmu);
	if (ret)
		return ret;

	/* Stream table */
	arm_smmu_write_strtab(smmu);

	/* Command queue */
	writeq_relaxed(smmu->cmdq.q.q_base, smmu->base + ARM_SMMU_CMDQ_BASE);

	return 0;
}

static int kvm_arm_smmu_probe(struct platform_device *pdev)
{
	int ret;
	size_t size;
	struct resource *res;
	struct arm_smmu_device *smmu;
	struct device *dev = &pdev->dev;
	struct host_arm_smmu_device *host_smmu;
	struct hyp_arm_smmu_v3_device_pv *hyp_smmu;

	if (kvm_arm_smmu_cur >= kvm_arm_smmu_count)
		return -ENOSPC;

	hyp_smmu = &kvm_arm_smmu_array[kvm_arm_smmu_cur];

	host_smmu = devm_kzalloc(dev, sizeof(*host_smmu), GFP_KERNEL);
	if (!host_smmu)
		return -ENOMEM;

	smmu = &host_smmu->smmu;
	smmu->dev = dev;

	ret = arm_smmu_fw_probe(pdev, smmu);
	if (ret)
		return ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = resource_size(res);
	if (size < SZ_128K) {
		dev_err(dev, "unsupported MMIO region size (%pr)\n", res);
		return -EINVAL;
	}
	host_smmu->ioaddr = res->start;
	host_smmu->id = kvm_arm_smmu_cur;

	smmu->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(smmu->base))
		return PTR_ERR(smmu->base);

	ret = arm_smmu_device_hw_probe(smmu);
	if (ret)
		return ret;

	if (!kvm_arm_smmu_validate_features(smmu))
		return -ENODEV;

	ret = arm_smmu_init_one_queue(smmu, &smmu->cmdq.q, smmu->base,
				      ARM_SMMU_CMDQ_PROD, ARM_SMMU_CMDQ_CONS,
				      CMDQ_ENT_DWORDS, "cmdq");
	if (ret)
		return ret;

	ret = arm_smmu_init_strtab(smmu);
	if (ret)
		return ret;

	ret = kvm_arm_smmu_device_reset(host_smmu);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, smmu);

	/* Hypervisor parameters */
	hyp_smmu->common.cmdq = smmu->cmdq.q;
	hyp_smmu->common.strtab_cfg = smmu->strtab_cfg;
	hyp_smmu->common.pgsize_bitmap = smmu->pgsize_bitmap;
	hyp_smmu->common.oas = smmu->oas;
	hyp_smmu->common.ias = smmu->ias;
	hyp_smmu->common.mmio_addr = host_smmu->ioaddr;
	hyp_smmu->common.mmio_size = size;
	hyp_smmu->common.features = smmu->features;
	hyp_smmu->ssid_bits = smmu->ssid_bits;
	kvm_arm_smmu_cur++;

	return 0;
}

static void kvm_arm_smmu_remove(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	/*
	 * There was an error during hypervisor setup. The hyp driver may
	 * have already enabled the device, so disable it.
	 */
	arm_smmu_device_disable(smmu);
	arm_smmu_update_gbpa(smmu, host_smmu->boot_gbpa, GBPA_ABORT);
	arm_smmu_unregister_iommu(smmu);
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

static int kvm_arm_smmu_array_alloc(void)
{
	int smmu_order;
	struct device_node *np;

	kvm_arm_smmu_count = 0;
	for_each_compatible_node(np, NULL, "arm,smmu-v3")
		kvm_arm_smmu_count++;

	if (!kvm_arm_smmu_count)
		return 0;

	/* Allocate the parameter list shared with the hypervisor */
	smmu_order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	kvm_arm_smmu_array = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
						      smmu_order);
	if (!kvm_arm_smmu_array)
		return -ENOMEM;

	return 0;
}

static void kvm_arm_smmu_array_free(void)
{
	int order;

	order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	free_pages((unsigned long)kvm_arm_smmu_array, order);
}

static int smmu_fin_device(struct device *dev, void *data)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	return arm_smmu_register_iommu(smmu, &kvm_arm_smmu_ops, host_smmu->ioaddr);
}

static int kvm_arm_smmu_v3_post_init(void)
{
	if (!kvm_arm_smmu_count)
		return 0;
	WARN_ON(driver_for_each_device(&kvm_arm_smmu_driver.driver, NULL,
		NULL, smmu_fin_device));
	return 0;
}

static int kvm_arm_smmu_v3_init_drv(void)
{
	int ret;

	ret = platform_driver_probe(&kvm_arm_smmu_driver, kvm_arm_smmu_probe);
	if (ret)
		goto err_free;

	if (kvm_arm_smmu_cur != kvm_arm_smmu_count) {
		/* A device exists but failed to probe */
		ret = -EUNATCH;
		goto err_free;
	}

#ifdef MODULE
	ret = pkvm_load_el2_module(kvm_nvhe_sym(smmu_init_hyp_module));
	if (ret) {
		pr_err("Failed to load SMMUv3 IOMMU EL2 module: %d\n", ret);
		goto err_free;
	}
#endif
	/*
	 * These variables are stored in the nVHE image, and won't be accessible
	 * after KVM initialization. Ownership of kvm_arm_smmu_array will be
	 * transferred to the hypervisor as well.
	 */
	kvm_hyp_arm_smmu_v3_pv_smmus = kvm_arm_smmu_array;
	kvm_hyp_arm_smmu_v3_pv_count = kvm_arm_smmu_count;

	ret = kvm_iommu_register_hyp_ops(ksym_ref_addr_nvhe(smmu_pv_ops));
	if (ret)
		goto err_free;

	return kvm_arm_smmu_v3_post_init();
err_free:
	kvm_arm_smmu_array_free();
	return ret;
}

static struct kvm_iommu_driver kvm_smmu_v3_ops = {
	.init_driver = kvm_arm_smmu_v3_init_drv,
};

static int kvm_arm_smmu_v3_register(void)
{
	int ret;

	if (!is_protected_kvm_enabled())
		return 0;

	ret = kvm_arm_smmu_array_alloc();
	if (ret || !kvm_arm_smmu_count)
		return ret;

	ret = kvm_iommu_register_driver(&kvm_smmu_v3_ops, smmu_hyp_pgt_pages());
	if (ret)
		kvm_arm_smmu_array_free();
	return ret;
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
