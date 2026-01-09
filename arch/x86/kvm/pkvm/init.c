// SPDX-License-Identifier: GPL-2.0
#include <linux/kvm_host.h>
#include <asm/kvm_pkvm.h>
#include "early_alloc.h"
#include "init.h"
#include "memory.h"
#include "mmu.h"

/*
 * Set by the host before deprivilege and used through the initialization
 * process.
 */
struct pkvm_init_ops *init_ops;

static void *hyp_pgt_base;
static void *host_pgt_base;
static void *pkvm_vmemmap_base;

static int divide_memory_pool(phys_addr_t phys, unsigned long size)
{
	unsigned long nr_pages;

	pkvm_early_alloc_init(__pkvm_va(phys), size);

	nr_pages = pkvm_hyp_pgtable_pages();
	hyp_pgt_base = pkvm_early_alloc_contig(nr_pages);
	if (!hyp_pgt_base)
		return -ENOMEM;

	nr_pages = pkvm_host_pgtable_pages();
	host_pgt_base = pkvm_early_alloc_contig(nr_pages);
	if (!host_pgt_base)
		return -ENOMEM;

	nr_pages = pkvm_vmemmap_pages(sizeof(struct pkvm_page));
	pkvm_vmemmap_base = pkvm_early_alloc_contig(nr_pages);
	if (!pkvm_vmemmap_base)
		return -ENOMEM;

	return 0;
}

static int back_vmemmap(phys_addr_t back_pa)
{
	unsigned long start_va, size, end_va = 0;
	struct memblock_region *reg;
	unsigned int i;
	int ret;

	/*
	 * Map the vmemmap region to virtual address at page 1.
	 * Keep page 0 unmapped, to catch NULL dereference bugs in the
	 * hypervisor code.
	 */
	__pkvm_vmemmap = PAGE_SIZE;

	for (i = 0; i < pkvm_memblock_nr; i++) {
		reg = &pkvm_memory[i];
		/* Translate a range of memory to vmemmap range */
		start_va = ALIGN_DOWN((unsigned long)pkvm_phys_to_page(reg->base),
				      PAGE_SIZE);
		/*
		 * The beginning of the pkvm_vmemmap region for the current
		 * memblock may already be backed by the page backing the end
		 * of the previous region, so avoid mapping it twice.
		 */
		start_va = max(start_va, end_va);

		end_va = ALIGN((unsigned long)pkvm_phys_to_page(reg->base + reg->size),
				PAGE_SIZE);
		/*
		 * The vmemmap va shall below PAGE_OFFSET to avoid overlapping
		 * with the pKVM direct mapping
		 */
		if (end_va >= PAGE_OFFSET)
			return -ENOMEM;
		if (start_va >= end_va)
			continue;

		size = end_va - start_va;
		/*
		 * Create mapping for vmemmap virtual address
		 * [start_va, start_va+size) to physical address
		 * [back_pa, back_pa+size).
		 */
		ret = pkvm_hyp_mmu_map(start_va, back_pa, size,
				       (u64)pgprot_val(PAGE_KERNEL));
		if (ret)
			return ret;

		memset(__pkvm_va(back_pa), 0, size);
		back_pa += size;
	}

	return 0;
}

static int create_hyp_mmu(const struct pkvm_mem_info infos[], int nr_infos)
{
	unsigned long nr_pages = pkvm_hyp_pgtable_pages();
	struct memblock_region *reg;
	unsigned int i;
	int ret;

	ret = pkvm_hyp_mmu_init(hyp_pgt_base, nr_pages);
	if (ret)
		return ret;

	/*
	 * Create mapping for the memory in memblocks, which includes all
	 * the memory host kernel can see, as well as the reserved memory
	 * for the pKVM hypervisor.
	 *
	 * The virtual address is the same with the kernel direct mapping.
	 */
	for (i = 0; i < pkvm_memblock_nr; i++) {
		reg = &pkvm_memory[i];
		ret = pkvm_hyp_mmu_map((unsigned long)__pkvm_va(reg->base), reg->base,
				       reg->size, (u64)pgprot_val(PAGE_KERNEL));
		if (ret)
			return ret;
	}

	/*
	 * Map pkvm's TEXT/DATA memory. The virtual address is the same
	 * with the kernel symbol mapping.
	 */
	for (i = 0; i < nr_infos; i++) {
		if (infos[i].type == PKVM_TEXT_DATA) {
			ret = pkvm_hyp_mmu_map(infos[i].va, infos[i].pa,
					       infos[i].size, infos[i].prot);
			if (ret)
				return ret;
		}
	}

	ret = back_vmemmap(__pkvm_pa(pkvm_vmemmap_base));
	if (ret)
		return ret;

	/* Load pKVM hypervisor's MMU to use pKVM vmemmap */
	pkvm_hyp_mmu_load();

	/* Update the pKVM mmu to use pKVM buddy allocator */
	return pkvm_hyp_mmu_switch_to_buddy(hyp_pgt_base, nr_pages);
}

static int initialize_global(struct pkvm_mem_info infos[], int nr_infos)
{
	host_mmu_init_fn_t host_mmu_init_fn = init_ops ? init_ops->host_mmu_init : NULL;
	phys_addr_t mem_base = INVALID_PAGE;
	unsigned long mem_size = 0;
	int i, ret;

	if (!infos || !nr_infos)
		return -EINVAL;

	for (i = 0; i < nr_infos; i++) {
		if (infos[i].type == PKVM_RESERVED_UNUSED_MEMORY) {
			mem_base = infos[i].pa;
			mem_size = infos[i].size;
			break;
		}
	}

	if (!PAGE_ALIGNED(mem_base) || !mem_size)
		return -EINVAL;

	ret = divide_memory_pool(mem_base, mem_size);
	if (ret)
		return ret;

	ret = create_hyp_mmu(infos, nr_infos);
	if (ret)
		return ret;

	return pkvm_host_mmu_init(host_pgt_base, pkvm_host_pgtable_pages(),
				  host_mmu_init_fn);
}

int pkvm_init(struct pkvm_mem_info infos[], int nr_infos)
{
	hyp_mmu_finalize_fn_t hyp_mmu_finalize_fn = init_ops ? init_ops->hyp_mmu_finalize :
							       NULL;
	static bool global_initialized;

	if (!global_initialized) {
		int ret = initialize_global(infos, nr_infos);

		if (ret)
			return ret;

		global_initialized = true;
	} else {
		/*
		 * The pKVM hypervisor's MMU was already loaded on the first
		 * initialized CPU during the global initialize. Need to load it
		 * on all the other CPUs as well.
		 */
		pkvm_hyp_mmu_load();
	}

	return pkvm_hyp_mmu_finalize(hyp_mmu_finalize_fn);
}
