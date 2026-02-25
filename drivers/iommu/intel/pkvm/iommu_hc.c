// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2026 Google.
 *
 */
#include <asm/kvm_pkvm.h>
#include <linux/pci.h>
#include "pkvm/mmu.h"
#include "pkvm/vmx/ept.h"
#include "pkvm/memory.h"
#include "pkvm/pkvm.h"
#include "pkvm/debug.h"
#include "../iommu.h"

int pkvm_iommu_qi_submit(u64 phys, u64 desc_gpa, u32 count, u32 options)
{
	struct intel_iommu *iommu = iommu_from_phys(phys);

	if (!iommu)
		return -EINVAL;

	BUG_ON(!iommu->qi);

	/*
	 * This hypercall is temporary so don't bother to write protect
	 * desc_gpa (host to hyp donation). It will be removed in future
	 * patches being replaced by a dedicated hypercall specifically
	 * for submitting QI_IEC_TYPE.
	 */
	return qi_submit_sync(iommu, pkvm_host_gpa_to_virt(desc_gpa),
			      count, options);
}

int pkvm_iommu_clear_ce(struct clear_ce_data *data)
{
	struct intel_iommu *iommu = iommu_from_phys(data->phys);
	u16 bdf = PCI_DEVID(data->bus, data->devfn);
	struct device_domain_info info = { 0 };

	if (!iommu)
		return -EINVAL;

	if (data->ats_qdep > PCI_ATS_MAX_QDEP)
		return -EINVAL;

	if (is_dev_in_satc(bdf)) {
		/*
		 * Device is in SATC and optimistically assuming that a well crafted SATC
		 * would contain only physical functions, its safe to set pfsid = bdf.
		 * TODO: We should probably be verifying SATC for existence of only
		 * physical functions during pkvm initialization.
		 */
		if (ecap_dit(iommu->ecap))
			info.pfsid = bdf;
	} else if (data->ats_enabled || data->ats_supported) {
		return -EPERM;
	}

	info.iommu = iommu;
	info.bus = data->bus;
	info.devfn = data->devfn;
	info.ats_qdep = data->ats_qdep;
	info.ats_supported = data->ats_supported;
	info.ats_enabled = data->ats_enabled;

	pkvm_dbg("%s: dev[%x:%x], ats_qdep: %d\n",
		 __func__, data->bus, data->devfn, data->ats_qdep);
	domain_context_clear_one(&info, data->bus, data->devfn);

	return 0;
}

static int accept_page_donation(struct intel_iommu *iommu, u64 *donation_page_gpa)
{
	pkvm_spin_lock(&iommu->lock);
	if (*donation_page_gpa && !iommu->donation_page) {
		u64 donation_page_pa = pkvm_host_gpa_to_phys(*donation_page_gpa);

		int ret = pkvm_host_donate_hyp_share_ro(donation_page_pa,
							VTD_PAGE_SIZE, true);

		if (ret) {
			pkvm_spin_unlock(&iommu->lock);
			pkvm_err("iommu%d: failed to write protect donated page(err=%d)!\n",
				 iommu->seq_id, ret);
			return ret;
		}
		iommu->donation_page = __pkvm_va(donation_page_pa);
		*donation_page_gpa = 0ULL;
	}
	pkvm_spin_unlock(&iommu->lock);

	return 0;
}

static int iommu_set_lm_ce(struct set_lm_ce_data *data)
{
	struct intel_iommu *iommu = iommu_from_phys(data->phys);
	u16 bdf = PCI_DEVID(data->bus, data->devfn);
	struct device_domain_info info = { 0 };
	struct dmar_domain domain = { 0 };
	int ret;

	if (!iommu)
		return -EINVAL;

	if (data->ats_qdep > PCI_ATS_MAX_QDEP)
		return -EINVAL;

	if (is_dev_in_satc(bdf)) {
		if (ecap_dit(iommu->ecap))
			info.pfsid = bdf;
	} else if (data->ats_enabled || data->ats_supported) {
		return -EPERM;
	}

	info.bus = data->bus;
	info.devfn = data->devfn;
	info.iommu = iommu;
	info.ats_qdep = data->ats_qdep;
	info.ats_supported = data->ats_supported;
	info.ats_enabled = data->ats_enabled;
	if (data->did == FLPT_DEFAULT_DID) {
		/*
		 * Passthrough will break pkvm security guarantees as
		 * device would be able to access the whole physical
		 * memory range. Use Second stage translation with host ept
		 * as second stage pagetable so as to limit device access
		 * to host memory.
		 */
		domain.pgd = __pkvm_va(pkvm_host_ept_root());
		domain.agaw = level_to_agaw(pkvm_host_ept_level());
	} else {
		domain.pgd = pkvm_host_gpa_to_virt(data->pgd_gpa);
		domain.agaw = iommu->agaw;
	}

	ret = accept_page_donation(iommu, &data->donation_page_gpa);
	if (ret)
		return ret;

	pkvm_dbg("%s: dev[%x:%x], did: %d, pgd: %p, agaw: %d\n", __func__,
		 data->bus, data->devfn, data->did, domain.pgd, domain.agaw);
	return domain_context_mapping_one(&domain, iommu, &info, data->did,
					  data->bus, data->devfn);
}

int pkvm_iommu_set_lm_ce(struct set_lm_ce_data *in, struct set_lm_ce_data *out)
{
	int ret = iommu_set_lm_ce(in);

	*out = *in;
	return ret;
}
