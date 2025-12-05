// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 - Google LLC
 * Author: Quentin Perret <qperret@google.com>
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/interval_tree_generic.h>
#include <linux/kmemleak.h>
#include <linux/kvm_host.h>
#include <asm/kvm_mmu.h>
#include <linux/memblock.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/sort.h>

#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>
#include <asm/kvm_pkvm_module.h>
#include <asm/text-patching.h>
#include <asm/setup.h>

#include "hyp_constants.h"
#include "hyp_trace.h"

DEFINE_STATIC_KEY_FALSE(kvm_protected_mode_initialized);

static struct reserved_mem *pkvm_firmware_mem;
static phys_addr_t *pvmfw_base = &kvm_nvhe_sym(pvmfw_base);
static phys_addr_t *pvmfw_size = &kvm_nvhe_sym(pvmfw_size);

static struct pkvm_moveable_reg *moveable_regs = kvm_nvhe_sym(pkvm_moveable_regs);
static struct memblock_region *hyp_memory = kvm_nvhe_sym(hyp_memory);
static unsigned int *hyp_memblock_nr_ptr = &kvm_nvhe_sym(hyp_memblock_nr);

phys_addr_t hyp_mem_base;
phys_addr_t hyp_mem_size;

static int __init register_memblock_regions(void)
{
	struct memblock_region *reg;

	for_each_mem_region(reg) {
		if (*hyp_memblock_nr_ptr >= HYP_MEMBLOCK_REGIONS)
			return -ENOMEM;

		hyp_memory[*hyp_memblock_nr_ptr] = *reg;
		(*hyp_memblock_nr_ptr)++;
	}

	return 0;
}

static int cmp_moveable_reg(const void *p1, const void *p2)
{
	const struct pkvm_moveable_reg *r1 = p1;
	const struct pkvm_moveable_reg *r2 = p2;

	/*
	 * Moveable regions may overlap, so put the largest one first when start
	 * addresses are equal to allow a simpler walk from e.g.
	 * host_stage2_unmap_unmoveable_regs().
	 */
	if (r1->start < r2->start)
		return -1;
	else if (r1->start > r2->start)
		return 1;
	else if (r1->size > r2->size)
		return -1;
	else if (r1->size < r2->size)
		return 1;
	return 0;
}

static void __init sort_moveable_regs(void)
{
	sort(moveable_regs,
	     kvm_nvhe_sym(pkvm_moveable_regs_nr),
	     sizeof(struct pkvm_moveable_reg),
	     cmp_moveable_reg,
	     NULL);
}

static int __init register_moveable_regions(void)
{
	struct memblock_region *reg;
	struct device_node *np;
	int i = 0;

	for_each_mem_region(reg) {
		if (i >= PKVM_NR_MOVEABLE_REGS)
			return -ENOMEM;
		moveable_regs[i].start = reg->base;
		moveable_regs[i].size = reg->size;
		moveable_regs[i].type = PKVM_MREG_MEMORY;
		i++;
	}

	for_each_compatible_node(np, NULL, "pkvm,protected-region") {
		struct resource res;
		u64 start, size;
		int ret;

		if (i >= PKVM_NR_MOVEABLE_REGS)
			return -ENOMEM;

		ret = of_address_to_resource(np, 0, &res);
		if (ret)
			return ret;

		start = res.start;
		size = resource_size(&res);
		if (!PAGE_ALIGNED(start) || !PAGE_ALIGNED(size))
			return -EINVAL;

		moveable_regs[i].start = start;
		moveable_regs[i].size = size;
		moveable_regs[i].type = PKVM_MREG_PROTECTED_RANGE;
		i++;
	}

	kvm_nvhe_sym(pkvm_moveable_regs_nr) = i;
	sort_moveable_regs();

	return 0;
}

void __init kvm_hyp_reserve(void)
{
	u64 hyp_mem_pages = 0;
	int ret;

	if (!is_hyp_mode_available() || is_kernel_in_hyp_mode())
		return;

	if (kvm_get_mode() != KVM_MODE_PROTECTED)
		return;

	ret = register_memblock_regions();
	if (ret) {
		*hyp_memblock_nr_ptr = 0;
		kvm_err("Failed to register hyp memblocks: %d\n", ret);
		return;
	}

	ret = register_moveable_regions();
	if (ret) {
		*hyp_memblock_nr_ptr = 0;
		kvm_err("Failed to register pkvm moveable regions: %d\n", ret);
		return;
	}

	hyp_mem_pages += hyp_s1_pgtable_pages();
	hyp_mem_pages += host_s2_pgtable_pages();
	hyp_mem_pages += hyp_vm_table_pages();
	hyp_mem_pages += hyp_vmemmap_pages(STRUCT_HYP_PAGE_SIZE);
	hyp_mem_pages += pkvm_selftest_pages();
	hyp_mem_pages += hyp_ffa_proxy_pages();

	/*
	 * Try to allocate a PMD-aligned region to reduce TLB pressure once
	 * this is unmapped from the host stage-2, and fallback to PAGE_SIZE.
	 */
	hyp_mem_size = hyp_mem_pages << PAGE_SHIFT;
	hyp_mem_base = memblock_phys_alloc(ALIGN(hyp_mem_size, PMD_SIZE),
					   PMD_SIZE);
	if (!hyp_mem_base)
		hyp_mem_base = memblock_phys_alloc(hyp_mem_size, PAGE_SIZE);
	else
		hyp_mem_size = ALIGN(hyp_mem_size, PMD_SIZE);

	if (!hyp_mem_base) {
		kvm_err("Failed to reserve hyp memory\n");
		return;
	}

	kvm_info("Reserved %lld MiB at 0x%llx\n", hyp_mem_size >> 20,
		 hyp_mem_base);
}

static void __pkvm_finalize_destroy_hyp_vm(struct kvm *kvm)
{
	if (pkvm_hyp_vm_is_created(kvm)) {
		WARN_ON(kvm_call_hyp_nvhe(__pkvm_finalize_teardown_vm,
					  kvm->arch.pkvm.handle));
	} else if (kvm->arch.pkvm.handle) {
		/*
		 * The VM could have been reserved but hyp initialization has
		 * failed. Make sure to unreserve it.
		 */
		kvm_call_hyp_nvhe(__pkvm_unreserve_vm, kvm->arch.pkvm.handle);
	}

	kvm->arch.pkvm.handle = 0;
	kvm->arch.pkvm.is_created = false;
	free_hyp_memcache(&kvm->arch.pkvm.stage2_teardown_mc);
}


static void __pkvm_vcpu_hyp_created(struct kvm_vcpu *vcpu)
{
	vcpu_set_flag(vcpu, VCPU_PKVM_FINALIZED);
}

static int __pkvm_create_hyp_vcpu(struct kvm_vcpu *vcpu)
{
	pkvm_handle_t handle = vcpu->kvm->arch.pkvm.handle;
	int ret;

	vcpu->arch.pkvm_memcache.flags |= HYP_MEMCACHE_ACCOUNT_STAGE2;

	ret = kvm_call_refill_hyp_nvhe(__pkvm_init_vcpu, handle, vcpu);
	if (!ret)
		__pkvm_vcpu_hyp_created(vcpu);

	return ret;
}

/*
 * Allocates and donates memory for hypervisor VM structs at EL2.
 *
 * Allocates space for the VM state, which includes the hyp vm as well as
 * the hyp vcpus.
 *
 * Stores an opaque handler in the kvm struct for future reference.
 *
 * Return 0 on success, negative error code on failure.
 */
static int __pkvm_create_hyp_vm(struct kvm *kvm)
{
	size_t pgd_sz;
	void *pgd;
	int ret;

	if (kvm->created_vcpus < 1)
		return -EINVAL;

	pgd_sz = kvm_pgtable_stage2_pgd_size(kvm->arch.mmu.vtcr);

	/*
	 * The PGD pages will be reclaimed using a hyp_memcache which implies
	 * page granularity. So, use alloc_pages_exact() to get individual
	 * refcounts.
	 */
	pgd = alloc_pages_exact(pgd_sz, GFP_KERNEL_ACCOUNT);
	if (!pgd)
		return -ENOMEM;

	ret = kvm_call_refill_hyp_nvhe(__pkvm_init_vm, kvm, pgd);
	if (ret)
		goto free_pgd;

	kvm->arch.pkvm.is_created = true;
	kvm->arch.pkvm.stage2_teardown_mc.flags |= HYP_MEMCACHE_ACCOUNT_STAGE2;
	kvm_account_pgtable_pages(pgd, pgd_sz / PAGE_SIZE);

	return 0;
free_pgd:
	free_pages_exact(pgd, pgd_sz);
	return ret;
}

bool pkvm_hyp_vm_is_created(struct kvm *kvm)
{
	return READ_ONCE(kvm->arch.pkvm.is_created);
}

int pkvm_create_hyp_vm(struct kvm *kvm)
{
	int ret = 0;

	mutex_lock(&kvm->arch.config_lock);
	if (!pkvm_hyp_vm_is_created(kvm))
		ret = __pkvm_create_hyp_vm(kvm);
	mutex_unlock(&kvm->arch.config_lock);

	return ret;
}

int pkvm_create_hyp_vcpu(struct kvm_vcpu *vcpu)
{
	int ret = 0;

	mutex_lock(&vcpu->kvm->arch.config_lock);
	if (!vcpu_get_flag(vcpu, VCPU_PKVM_FINALIZED))
		ret = __pkvm_create_hyp_vcpu(vcpu);
	mutex_unlock(&vcpu->kvm->arch.config_lock);

	return ret;
}

void pkvm_finalize_destroy_hyp_vm(struct kvm *kvm)
{
	mutex_lock(&kvm->arch.config_lock);
	__pkvm_finalize_destroy_hyp_vm(kvm);
	mutex_unlock(&kvm->arch.config_lock);
}

int pkvm_init_host_vm(struct kvm *kvm, unsigned long type)
{
	int ret;

	if (!is_protected_kvm_enabled())
		return -EINVAL;

	if (pkvm_hyp_vm_is_created(kvm))
		return -EINVAL;

	/* VM is already reserved, no need to proceed. */
	if (kvm->arch.pkvm.handle)
		return 0;

	/* Reserve the VM in hyp and obtain a hyp handle for the VM. */
	ret = kvm_call_hyp_nvhe(__pkvm_reserve_vm);
	if (ret < 0)
		return ret;

	kvm->arch.pkvm.handle = ret;
	kvm->arch.pkvm.is_protected = (type & KVM_VM_TYPE_ARM_PROTECTED);
	kvm->arch.pkvm.pvmfw_load_addr = PVMFW_INVALID_LOAD_ADDR;

	return 0;
}

static void __init _kvm_host_prot_finalize(void *arg)
{
	int *err = arg;

	if (WARN_ON(kvm_call_hyp_nvhe(__pkvm_prot_finalize)))
		WRITE_ONCE(*err, -EINVAL);
}

static int __init pkvm_drop_host_privileges(void)
{
	int ret = 0;

	/*
	 * Flip the static key upfront as that may no longer be possible
	 * once the host stage 2 is installed.
	 */
	static_branch_enable(&kvm_protected_mode_initialized);
	on_each_cpu(_kvm_host_prot_finalize, &ret, 1);
	return ret;
}

static int __init pkvm_firmware_rmem_clear(void);

static int __init finalize_pkvm(void)
{
	int ret;

	if (!is_protected_kvm_enabled() || !is_kvm_arm_initialised()) {
		pkvm_firmware_rmem_clear();
		return 0;
	}

	/*
	 * Modules can play an essential part in the pKVM protection. All of
	 * them must properly load to enable protected VMs.
	 */
	if (pkvm_load_early_modules())
		pkvm_firmware_rmem_clear();

	/*
	 * Exclude HYP sections from kmemleak so that they don't get peeked
	 * at, which would end badly once inaccessible.
	 */
	kmemleak_free_part(__hyp_bss_start, __hyp_bss_end - __hyp_bss_start);
	kmemleak_free_part(__hyp_data_start, __hyp_data_end - __hyp_data_start);
	kmemleak_free_part(__hyp_rodata_start, __hyp_rodata_end - __hyp_rodata_start);
	kmemleak_free_part_phys(hyp_mem_base, hyp_mem_size);

	ret = pkvm_drop_host_privileges();
	if (ret) {
		pr_err("Failed to finalize Hyp protection: %d\n", ret);
		BUG();
	}

	return 0;
}
device_initcall_sync(finalize_pkvm);

static int __init pkvm_firmware_rmem_err(struct reserved_mem *rmem,
					 const char *reason)
{
	phys_addr_t end = rmem->base + rmem->size;

	kvm_err("Ignoring pkvm guest firmware memory reservation [%pa - %pa]: %s\n",
		&rmem->base, &end, reason);
	return -EINVAL;
}

static int __init pkvm_firmware_rmem_init(struct reserved_mem *rmem)
{
	unsigned long node = rmem->fdt_node;

	if (pkvm_firmware_mem)
		return pkvm_firmware_rmem_err(rmem, "duplicate reservation");

	if (!of_get_flat_dt_prop(node, "no-map", NULL))
		return pkvm_firmware_rmem_err(rmem, "missing \"no-map\" property");

	if (of_get_flat_dt_prop(node, "reusable", NULL))
		return pkvm_firmware_rmem_err(rmem, "\"reusable\" property unsupported");

	if (!PAGE_ALIGNED(rmem->base))
		return pkvm_firmware_rmem_err(rmem, "base is not page-aligned");

	if (!PAGE_ALIGNED(rmem->size))
		return pkvm_firmware_rmem_err(rmem, "size is not page-aligned");

	*pvmfw_size = rmem->size;
	*pvmfw_base = rmem->base;
	pkvm_firmware_mem = rmem;
	return 0;
}
RESERVEDMEM_OF_DECLARE(pkvm_firmware, "linux,pkvm-guest-firmware-memory",
		       pkvm_firmware_rmem_init);

static int __init pkvm_firmware_rmem_clear(void)
{
	void *addr;
	phys_addr_t size;

	if (likely(!pkvm_firmware_mem))
		return 0;

	kvm_info("Clearing pKVM firmware memory\n");
	size = pkvm_firmware_mem->size;
	addr = memremap(pkvm_firmware_mem->base, size, MEMREMAP_WB);
	if (!addr)
		return -EINVAL;

	memset(addr, 0, size);
	dcache_clean_poc((unsigned long)addr, (unsigned long)addr + size);
	memunmap(addr);
	return 0;
}

static int pkvm_vm_ioctl_set_fw_ipa(struct kvm *kvm, u64 ipa)
{
	int ret = 0;

	if (!pkvm_firmware_mem)
		return -EINVAL;

	mutex_lock(&kvm->lock);
	if (pkvm_hyp_vm_is_created(kvm)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	kvm->arch.pkvm.pvmfw_load_addr = ipa;
out_unlock:
	mutex_unlock(&kvm->lock);
	return ret;
}

static int pkvm_vm_ioctl_info(struct kvm *kvm,
			      struct kvm_protected_vm_info __user *info)
{
	struct kvm_protected_vm_info kinfo = {
		.firmware_size = pkvm_firmware_mem ?
				 pkvm_firmware_mem->size :
				 0,
	};

	return copy_to_user(info, &kinfo, sizeof(kinfo)) ? -EFAULT : 0;
}

int pkvm_vm_ioctl_enable_cap(struct kvm *kvm, struct kvm_enable_cap *cap)
{
	if (!kvm_vm_is_protected(kvm))
		return -EINVAL;

	if (cap->args[1] || cap->args[2] || cap->args[3])
		return -EINVAL;

	switch (cap->flags) {
	case KVM_CAP_ARM_PROTECTED_VM_FLAGS_SET_FW_IPA:
		return pkvm_vm_ioctl_set_fw_ipa(kvm, cap->args[0]);
	case KVM_CAP_ARM_PROTECTED_VM_FLAGS_INFO:
		return pkvm_vm_ioctl_info(kvm, (void __force __user *)cap->args[0]);
	default:
		return -EINVAL;
	}

	return 0;
}

static u64 __pkvm_mapping_start(struct pkvm_mapping *m)
{
	return m->gfn * PAGE_SIZE;
}

static u64 __pkvm_mapping_end(struct pkvm_mapping *m)
{
	return (m->gfn + m->nr_pages) * PAGE_SIZE - 1;
}

INTERVAL_TREE_DEFINE(struct pkvm_mapping, node, u64, __subtree_last,
		     __pkvm_mapping_start, __pkvm_mapping_end, static,
		     pkvm_mapping);

/*
 * __tmp is updated to iter_first(pkvm_mappings) *before* entering the body of the loop to allow
 * freeing of __map inline.
 */
#define for_each_mapping_in_range_safe(__pgt, __start, __end, __map)				\
	for (struct pkvm_mapping *__tmp = pkvm_mapping_iter_first(&(__pgt)->pkvm_mappings,	\
								  __start, __end - 1);		\
	     __tmp && ({									\
				__map = __tmp;							\
				__tmp = pkvm_mapping_iter_next(__map, __start, __end - 1);	\
				true;								\
		       });									\
	    )

int pkvm_pgtable_stage2_init(struct kvm_pgtable *pgt, struct kvm_s2_mmu *mmu,
			     struct kvm_pgtable_mm_ops *mm_ops)
{
	pgt->pkvm_mappings	= RB_ROOT_CACHED;
	pgt->mmu		= mmu;

	return 0;
}

void pkvm_host_reclaim_page(struct kvm *kvm, phys_addr_t ipa)
{
	struct kvm_pgtable *pgt = kvm->arch.mmu.pgt;
	struct pkvm_mapping *mapping = NULL;
	struct mm_struct *mm = current->mm;
	struct page *page;

	write_lock(&kvm->mmu_lock);
	mapping = pkvm_mapping_iter_first(&pgt->pkvm_mappings, ipa, ipa + PAGE_SIZE - 1);
	if (mapping)
		pkvm_mapping_remove(mapping, &pgt->pkvm_mappings);
	write_unlock(&kvm->mmu_lock);

	if (WARN_ON(!mapping))
		return;

	account_locked_vm(mm, 1, false);
	page = pfn_to_page(mapping->pfn);
	unpin_user_pages_dirty_lock(&page, 1, true);
	kfree(mapping);
}

static int __pkvm_pgtable_stage2_unmap(struct kvm_pgtable *pgt, u64 start, u64 end)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;
	struct pkvm_mapping *mapping;

	if (!handle)
		return 0;

	for_each_mapping_in_range_safe(pgt, start, end, mapping)
	{
		int ret;

		if (kvm_vm_is_protected(kvm))
			ret = kvm_call_hyp_nvhe(__pkvm_reclaim_dying_guest_page, handle,
						mapping->gfn);
		else
			ret = kvm_call_hyp_nvhe(__pkvm_host_unshare_guest, handle,
						mapping->gfn, mapping->nr_pages);

		if (WARN_ON(ret))
			return ret;

		pkvm_mapping_remove(mapping, &pgt->pkvm_mappings);
		kfree(mapping);
	}

	/* The finalization of the VM teardown is done from kvm_arch_destroy_vm() */

	return 0;
}

void pkvm_pgtable_stage2_destroy(struct kvm_pgtable *pgt)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;

	WARN_ON(kvm_call_hyp_nvhe(__pkvm_start_teardown_vm, handle));

	__pkvm_pgtable_stage2_unmap(pgt, 0, ~(0ULL));
}

int pkvm_pgtable_stage2_map(struct kvm_pgtable *pgt, u64 addr, u64 size,
			   u64 phys, enum kvm_pgtable_prot prot,
			   void *mc, enum kvm_pgtable_walk_flags flags)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	struct pkvm_mapping *mapping = NULL;
	struct kvm_hyp_memcache *cache = mc;
	u64 gfn = addr >> PAGE_SHIFT;
	u64 pfn = phys >> PAGE_SHIFT;
	int ret;

	if (size != PAGE_SIZE && size != PMD_SIZE)
		return -EINVAL;

	lockdep_assert_held_write(&kvm->mmu_lock);

	/*
	 * Calling stage2_map() on top of existing mappings is either happening because of a race
	 * with another vCPU, or because we're changing between page and block mappings. As per
	 * user_mem_abort(), same-size permission faults are handled in the relax_perms() path.
	 */
	mapping = pkvm_mapping_iter_first(&pgt->pkvm_mappings, addr, addr + size - 1);
	if (mapping) {
		if (size == (mapping->nr_pages * PAGE_SIZE))
			return -EAGAIN;

		/* Remove _any_ pkvm_mapping overlapping with the range, bigger or smaller. */
		ret = __pkvm_pgtable_stage2_unmap(pgt, addr, addr + size);
		if (ret)
			return ret;
		mapping = NULL;
	}

	ret = kvm_call_hyp_nvhe(__pkvm_host_map_guest, pfn, gfn, size / PAGE_SIZE, prot);
	if (WARN_ON(ret))
		return ret;

	swap(mapping, cache->mapping);
	mapping->gfn = gfn;
	mapping->pfn = pfn;
	mapping->nr_pages = size / PAGE_SIZE;
	pkvm_mapping_insert(mapping, &pgt->pkvm_mappings);

	return ret;
}

int pkvm_pgtable_stage2_unmap(struct kvm_pgtable *pgt, u64 addr, u64 size)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);

	if (WARN_ON(kvm_vm_is_protected(kvm)))
		return -EPERM;

	lockdep_assert_held_write(&kvm->mmu_lock);

	return __pkvm_pgtable_stage2_unmap(pgt, addr, addr + size);
}

int pkvm_pgtable_stage2_wrprotect(struct kvm_pgtable *pgt, u64 addr, u64 size)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;
	struct pkvm_mapping *mapping;
	int ret = 0;

	if (WARN_ON(kvm_vm_is_protected(kvm)))
		return -EPERM;

	lockdep_assert_held(&kvm->mmu_lock);
	for_each_mapping_in_range_safe(pgt, addr, addr + size, mapping) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_wrprotect_guest, handle, mapping->gfn,
					mapping->nr_pages);
		if (WARN_ON(ret))
			break;
	}

	return ret;
}

int pkvm_pgtable_stage2_flush(struct kvm_pgtable *pgt, u64 addr, u64 size)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	struct pkvm_mapping *mapping;

	lockdep_assert_held(&kvm->mmu_lock);
	for_each_mapping_in_range_safe(pgt, addr, addr + size, mapping)
		__clean_dcache_guest_page(pfn_to_kaddr(mapping->pfn),
					  PAGE_SIZE * mapping->nr_pages);

	return 0;
}

bool pkvm_pgtable_stage2_test_clear_young(struct kvm_pgtable *pgt, u64 addr, u64 size, bool mkold)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;
	struct pkvm_mapping *mapping;
	bool young = false;

	if (WARN_ON(kvm_vm_is_protected(kvm)))
		return -EPERM;

	lockdep_assert_held(&kvm->mmu_lock);
	for_each_mapping_in_range_safe(pgt, addr, addr + size, mapping)
		young |= kvm_call_hyp_nvhe(__pkvm_host_test_clear_young_guest, handle, mapping->gfn,
					   mapping->nr_pages, mkold);

	return young;
}

int pkvm_pgtable_stage2_relax_perms(struct kvm_pgtable *pgt, u64 addr, enum kvm_pgtable_prot prot,
				    enum kvm_pgtable_walk_flags flags)
{
	if (WARN_ON(kvm_vm_is_protected(kvm_s2_mmu_to_kvm(pgt->mmu))))
		return -EPERM;

	return kvm_call_hyp_nvhe(__pkvm_host_relax_perms_guest, addr >> PAGE_SHIFT, prot);
}

void pkvm_pgtable_stage2_mkyoung(struct kvm_pgtable *pgt, u64 addr,
				 enum kvm_pgtable_walk_flags flags)
{
	if (WARN_ON(kvm_vm_is_protected(kvm_s2_mmu_to_kvm(pgt->mmu))))
		return;

	WARN_ON(kvm_call_hyp_nvhe(__pkvm_host_mkyoung_guest, addr >> PAGE_SHIFT));
}

void pkvm_pgtable_stage2_free_unlinked(struct kvm_pgtable_mm_ops *mm_ops, void *pgtable, s8 level)
{
	WARN_ON_ONCE(1);
}

kvm_pte_t *pkvm_pgtable_stage2_create_unlinked(struct kvm_pgtable *pgt, u64 phys, s8 level,
					enum kvm_pgtable_prot prot, void *mc, bool force_pte)
{
	WARN_ON_ONCE(1);
	return NULL;
}

int pkvm_pgtable_stage2_split(struct kvm_pgtable *pgt, u64 addr, u64 size,
			      struct kvm_mmu_memory_cache *mc)
{
	WARN_ON_ONCE(1);
	return -EINVAL;
}

#ifdef CONFIG_MODULES
static char early_pkvm_modules[COMMAND_LINE_SIZE] __initdata;

static int __init early_pkvm_modules_cfg(char *arg)
{
	/*
	 * Loading pKVM modules with kvm-arm.protected_modules is deprecated
	 * Use kvm-arm.protected_modules=<module1>,<module2>
	 */
	if (!arg)
		return -EINVAL;

	strscpy(early_pkvm_modules, arg, COMMAND_LINE_SIZE);

	return 0;
}
early_param("kvm-arm.protected_modules", early_pkvm_modules_cfg);

static void free_modprobe_argv(struct subprocess_info *info)
{
	kfree(info->argv);
}

/*
 * Heavily inspired by request_module(). The latest couldn't be reused though as
 * the feature can be disabled depending on umh configuration. Here some
 * security is enforced by making sure this can be called only when pKVM is
 * enabled, not yet completely initialized.
 */
static int __init __pkvm_request_early_module(char *module_name,
					      char *module_path)
{
	char *modprobe_path = CONFIG_MODPROBE_PATH;
	struct subprocess_info *info;
	static char *envp[] = {
		"HOME=/",
		"TERM=linux",
		"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
		NULL
	};
	char **argv;
	int idx = 0;

	if (!is_protected_kvm_enabled())
		return -EACCES;

	if (static_branch_likely(&kvm_protected_mode_initialized))
		return -EACCES;

	argv = kmalloc(sizeof(char *) * 7, GFP_KERNEL);
	if (!argv)
		return -ENOMEM;

	argv[idx++] = modprobe_path;
	argv[idx++] = "-q";
	if (*module_path != '\0') {
		argv[idx++] = "-d";
		argv[idx++] = module_path;
	}
	argv[idx++] = "--";
	argv[idx++] = module_name;
	argv[idx++] = NULL;

	info = call_usermodehelper_setup(modprobe_path, argv, envp, GFP_KERNEL,
					 NULL, free_modprobe_argv, NULL);
	if (!info)
		goto err;

	/* Even with CONFIG_STATIC_USERMODEHELPER we really want this path */
	info->path = modprobe_path;

	return call_usermodehelper_exec(info, UMH_WAIT_PROC | UMH_KILLABLE);
err:
	kfree(argv);

	return -ENOMEM;
}

static int __init pkvm_request_early_module(char *module_name, char *module_path)
{
	int err = __pkvm_request_early_module(module_name, module_path);

	if (!err)
		return 0;

	/* Already tried the default path */
	if (*module_path == '\0')
		return err;

	pr_info("loading %s from %s failed, fallback to the default path\n",
		module_name, module_path);

	return __pkvm_request_early_module(module_name, "");
}

static void pkvm_el2_mod_free(void);

int __init pkvm_load_early_modules(void)
{
	char *token, *buf = early_pkvm_modules;
	char *module_path = CONFIG_PKVM_MODULE_PATH;
	int err = 0;

	while (true) {
		token = strsep(&buf, ",");

		if (!token)
			break;

		if (*token) {
			err = pkvm_request_early_module(token, module_path);
			if (err) {
				pr_err("Failed to load pkvm module %s: %d\n",
				       token, err);
				goto out;
			}
		}

		if (buf)
			*(buf - 1) = ',';
	}

out:
	pkvm_el2_mod_free();

	return err;
}

static LIST_HEAD(pkvm_modules);

static void pkvm_el2_mod_add(struct pkvm_el2_module *mod)
{
	INIT_LIST_HEAD(&mod->node);
	list_add(&mod->node, &pkvm_modules);
}

static void pkvm_el2_mod_free(void)
{
	struct pkvm_el2_sym *sym, *tmp;
	struct pkvm_el2_module *mod;

	list_for_each_entry(mod, &pkvm_modules, node) {
		list_for_each_entry_safe(sym, tmp, &mod->ext_symbols, node) {
			list_del(&sym->node);
			kfree(sym->name);
			kfree(sym);
		}
	}
}

static struct module *pkvm_el2_mod_to_module(struct pkvm_el2_module *hyp_mod)
{
	struct mod_arch_specific *arch;

	arch = container_of(hyp_mod, struct mod_arch_specific, hyp);
	return container_of(arch, struct module, arch);
}

#ifdef CONFIG_PROTECTED_NVHE_STACKTRACE
unsigned long pkvm_el2_mod_kern_va(unsigned long addr)
{
	struct pkvm_el2_module *mod;

	list_for_each_entry(mod, &pkvm_modules, node) {
		unsigned long hyp_va = (unsigned long)mod->hyp_va;
		size_t len = (unsigned long)mod->sections.end -
			     (unsigned long)mod->sections.start;

		if (addr >= hyp_va && addr < (hyp_va + len))
			return (unsigned long)mod->sections.start +
				(addr - hyp_va);
	}

	return 0;
}
#else
unsigned long pkvm_el2_mod_kern_va(unsigned long addr) { return 0; }
#endif

static struct pkvm_el2_module *pkvm_el2_mod_lookup_symbol(const char *name,
							  unsigned long *addr)
{
	struct pkvm_el2_module *hyp_mod;
	unsigned long __addr;

	list_for_each_entry(hyp_mod, &pkvm_modules, node) {
		struct module *mod = pkvm_el2_mod_to_module(hyp_mod);

		__addr = find_kallsyms_symbol_value(mod, name);
		if (!__addr)
			continue;

		*addr = __addr;
		return hyp_mod;
	}

	return NULL;
}

static bool within_pkvm_module_section(struct pkvm_module_section *section,
				       unsigned long addr)
{
	return (addr >= (unsigned long)section->start) &&
		(addr < (unsigned long)section->end);
}

static int pkvm_reloc_imported_symbol(struct pkvm_el2_module *importer,
				      struct pkvm_el2_sym *sym,
				      unsigned long hyp_dst)
{
	s64 val, val_max = (s64)(~(BIT(25) - 1)) << 2;
	u32 insn = le32_to_cpu(*sym->rela_pos);
	unsigned long hyp_src;
	u64 imm;

	if (!within_pkvm_module_section(&importer->text,
					(unsigned long)sym->rela_pos))
		return -EINVAL;

	hyp_src = (unsigned long)importer->hyp_va +
		((void *)sym->rela_pos - importer->text.start);

	/*
	 * Module hyp VAs are allocated going upward. Source MUST have a
	 * lower address than the destination
	 */
	if (WARN_ON(hyp_src < hyp_dst))
		return -EINVAL;

	val = hyp_dst - hyp_src;
	if (val < val_max) {
		pr_warn("Exported symbol %s is too far for the relocation in module %s\n",
			sym->name, pkvm_el2_mod_to_module(importer)->name);
		return -ERANGE;
	}

	/* offset encoded as imm26 * 4 */
	imm = (val >> 2) & (BIT(26) - 1);

	insn = aarch64_insn_encode_immediate(AARCH64_INSN_IMM_26, insn, imm);

	return aarch64_insn_patch_text_nosync((void *)sym->rela_pos, insn);
}

static int pkvm_reloc_imported_symbols(struct pkvm_el2_module *importer)
{
	unsigned long addr, offset, hyp_addr;
	struct pkvm_el2_module *exporter;
	struct pkvm_el2_sym *sym;

	list_for_each_entry(sym, &importer->ext_symbols, node) {
		exporter = pkvm_el2_mod_lookup_symbol(sym->name, &addr);
		if (!exporter) {
			pr_warn("pKVM symbol %s not exported by any module\n",
				sym->name);
			return -EINVAL;
		}

		if (!within_pkvm_module_section(&exporter->text, addr)) {
			pr_warn("pKVM symbol %s not part of %s .text section\n",
				sym->name,
				pkvm_el2_mod_to_module(exporter)->name);
			return -EINVAL;
		}

		/* hyp addr in the exporter */
		offset = addr - (unsigned long)exporter->text.start;
		hyp_addr = (unsigned long)exporter->hyp_va + offset;

		pkvm_reloc_imported_symbol(importer, sym, hyp_addr);
	}

	return 0;
}

struct pkvm_mod_sec_mapping {
	struct pkvm_module_section *sec;
	enum kvm_pgtable_prot prot;
};

static void pkvm_unmap_module_pages(void *kern_va, void *hyp_va, size_t size)
{
	size_t offset;
	u64 pfn;

	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		pfn = vmalloc_to_pfn(kern_va + offset);
		kvm_call_hyp_nvhe(__pkvm_unmap_module_page, pfn,
				  hyp_va + offset);
	}
}

static void pkvm_unmap_module_sections(struct pkvm_mod_sec_mapping *secs_map, void *hyp_va_base, int nr_secs)
{
	size_t offset, size;
	void *start;
	int i;

	for (i = 0; i < nr_secs; i++) {
		start = secs_map[i].sec->start;
		size = secs_map[i].sec->end - start;
		offset = start - secs_map[0].sec->start;
		pkvm_unmap_module_pages(start, hyp_va_base + offset, size);
	}
}

static int pkvm_map_module_section(struct pkvm_mod_sec_mapping *sec_map, void *hyp_va)
{
	size_t offset, size = sec_map->sec->end - sec_map->sec->start;
	int ret;
	u64 pfn;

	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		pfn = vmalloc_to_pfn(sec_map->sec->start + offset);
		ret = kvm_call_hyp_nvhe(__pkvm_map_module_page, pfn,
					hyp_va + offset, sec_map->prot);
		if (ret) {
			pkvm_unmap_module_pages(sec_map->sec->start, hyp_va, offset);
			return ret;
		}
	}

	return 0;
}

static int pkvm_map_module_sections(struct pkvm_mod_sec_mapping *secs_map,
				    void *hyp_va_base, int nr_secs)
{
	size_t offset;
	int i, ret;

	for (i = 0; i < nr_secs; i++) {
		offset = secs_map[i].sec->start - secs_map[0].sec->start;
		ret = pkvm_map_module_section(&secs_map[i], hyp_va_base + offset);
		if (ret) {
			pkvm_unmap_module_sections(secs_map, hyp_va_base, i);
			return ret;
		}
	}

	return 0;
}
static int __pkvm_cmp_mod_sec(const void *p1, const void *p2)
{
	struct pkvm_mod_sec_mapping const *s1 = p1;
	struct pkvm_mod_sec_mapping const *s2 = p2;

	return s1->sec->start < s2->sec->start ? -1 : s1->sec->start > s2->sec->start;
}

int __pkvm_load_el2_module(struct module *this, unsigned long *token)
{
	struct pkvm_el2_module *mod = &this->arch.hyp;
	struct pkvm_mod_sec_mapping secs_map[] = {
		{ &mod->text, KVM_PGTABLE_PROT_R | KVM_PGTABLE_PROT_X },
		{ &mod->bss, KVM_PGTABLE_PROT_R | KVM_PGTABLE_PROT_W },
		{ &mod->rodata, KVM_PGTABLE_PROT_R },
		{ &mod->event_ids, KVM_PGTABLE_PROT_R },
		{ &mod->data, KVM_PGTABLE_PROT_R | KVM_PGTABLE_PROT_W },
	};
	void *start, *end, *hyp_va;
	struct arm_smccc_res res;
	kvm_nvhe_reloc_t *endrel;
	int ret, i, secs_first;
	size_t offset, size;

	/* The pKVM hyp only allows loading before it is fully initialized */
	if (!is_protected_kvm_enabled() || is_pkvm_initialized())
		return -EOPNOTSUPP;

	for (i = 0; i < ARRAY_SIZE(secs_map); i++) {
		if (!PAGE_ALIGNED(secs_map[i].sec->start)) {
			kvm_err("EL2 sections are not page-aligned\n");
			return -EINVAL;
		}
	}

	if (!try_module_get(this)) {
		kvm_err("Kernel module has been unloaded\n");
		return -ENODEV;
	}

	/* Missing or empty module sections are placed first */
	sort(secs_map, ARRAY_SIZE(secs_map), sizeof(secs_map[0]), __pkvm_cmp_mod_sec, NULL);
	for (secs_first = 0; secs_first < ARRAY_SIZE(secs_map); secs_first++) {
		start = secs_map[secs_first].sec->start;
		if (start)
			break;
	}
	end = secs_map[ARRAY_SIZE(secs_map) - 1].sec->end;
	size = end - start;

	mod->sections.start = start;
	mod->sections.end = end;

	arm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(__pkvm_alloc_module_va),
			  size >> PAGE_SHIFT, &res);
	if (res.a0 != SMCCC_RET_SUCCESS || !res.a1) {
		kvm_err("Failed to allocate hypervisor VA space for EL2 module\n");
		module_put(this);
		return res.a0 == SMCCC_RET_SUCCESS ? -ENOMEM : -EPERM;
	}
	hyp_va = (void *)res.a1;
	mod->hyp_va = hyp_va;

	/*
	 * The token can be used for other calls related to this module.
	 * Conveniently the only information needed is this addr so let's use it
	 * as an identifier.
	 */
	if (token)
		*token = (unsigned long)hyp_va;

	mod->sections.start = start;
	mod->sections.end = end;

	endrel = (void *)mod->relocs + mod->nr_relocs * sizeof(*endrel);
	kvm_apply_hyp_module_relocations(mod, mod->relocs, endrel);

	ret = pkvm_reloc_imported_symbols(mod);
	if (ret)
		return ret;

	/*
	 * Sadly we have also to disable kmemleak for EL1 sections: we can't
	 * reset created scan area and therefore we can't create a finer grain
	 * scan excluding only EL2 sections.
	 */
	if (this) {
		kmemleak_no_scan(this->mem[MOD_TEXT].base);
		kmemleak_no_scan(this->mem[MOD_DATA].base);
		kmemleak_no_scan(this->mem[MOD_RODATA].base);
	}

	ret = hyp_trace_init_mod_events(mod->hyp_events,
					mod->event_ids.start,
					mod->nr_hyp_events,
					mod->hyp_printk_fmts,
					mod->nr_hyp_printk_fmts);
	if (ret)
		kvm_err("Failed to init module events: %d\n", ret);

	ret = pkvm_map_module_sections(secs_map + secs_first, hyp_va,
				       ARRAY_SIZE(secs_map) - secs_first);
	if (ret) {
		kvm_err("Failed to map EL2 module page: %d\n", ret);
		module_put(this);
		return ret;
	}

	offset = (size_t)((void *)mod->init - start);
	ret = kvm_call_hyp_nvhe(__pkvm_init_module, hyp_va + offset);
	if (ret) {
		kvm_err("Failed to init EL2 module: %d\n", ret);
		pkvm_unmap_module_sections(secs_map, hyp_va, ARRAY_SIZE(secs_map));
		module_put(this);
		return ret;
	}

	hyp_trace_enable_event_early();

	pkvm_el2_mod_add(mod);

	return 0;
}
EXPORT_SYMBOL(__pkvm_load_el2_module);

int __pkvm_register_el2_call(unsigned long hfn_hyp_va)
{
	return kvm_call_hyp_nvhe(__pkvm_register_hcall, hfn_hyp_va);
}
EXPORT_SYMBOL(__pkvm_register_el2_call);
#endif /* CONFIG_MODULES */

int __pkvm_topup_hyp_alloc(unsigned long nr_pages)
{
	struct kvm_hyp_memcache mc = {
		.head		= 0,
		.nr_pages	= 0,
	};
	int ret;

	ret = topup_hyp_memcache(&mc, nr_pages);
	if (ret)
		return ret;

	ret = kvm_call_hyp_nvhe(__pkvm_hyp_alloc_refill, mc.head, mc.nr_pages);
	if (ret)
		free_hyp_memcache(&mc);

	return ret;
}
EXPORT_SYMBOL(__pkvm_topup_hyp_alloc);

unsigned long __pkvm_reclaim_hyp_alloc(unsigned long nr_pages)
{
	unsigned long ratelimit, last_reclaim, reclaimed = 0;
	struct kvm_hyp_memcache mc;
	struct arm_smccc_res res;

	do {
		/* Arbitrary upper bound to limit the time spent at EL2 */
		ratelimit = min(nr_pages, 16UL);

		arm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(__pkvm_hyp_alloc_reclaim),
				  ratelimit, &res);
		if (WARN_ON(res.a0 != SMCCC_RET_SUCCESS))
			break;

		mc.head = res.a1;
		last_reclaim = mc.nr_pages = res.a2;

		free_hyp_memcache(&mc);
		reclaimed += last_reclaim;

	} while (last_reclaim && (reclaimed < nr_pages));

	return reclaimed;
}
