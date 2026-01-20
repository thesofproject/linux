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

static int divide_memory_pool(phys_addr_t phys, unsigned long size)
{
	unsigned long nr_pages;

	pkvm_early_alloc_init(__pkvm_va(phys), size);

	nr_pages = pkvm_hyp_pgtable_pages();
	hyp_pgt_base = pkvm_early_alloc_contig(nr_pages);
	if (!hyp_pgt_base)
		return -ENOMEM;

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

	return 0;
}

static int initialize_global(struct pkvm_mem_info infos[], int nr_infos)
{
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

	return create_hyp_mmu(infos, nr_infos);
}

int pkvm_init(struct pkvm_mem_info infos[], int nr_infos)
{
	static bool global_initialized;

	if (!global_initialized) {
		int ret = initialize_global(infos, nr_infos);

		if (ret)
			return ret;

		global_initialized = true;
	}

	return 0;
}
