// SPDX-License-Identifier: GPL-2.0
#include <linux/cache.h>
#include <linux/types.h>
#include <asm/kvm_pkvm.h>

unsigned long page_offset_base __ro_after_init;
unsigned long phys_base;
#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
phys_addr_t physical_mask;
#endif
pteval_t __default_kernel_pte_mask;
#ifdef CONFIG_AMD_MEM_ENCRYPT
u64 sme_me_mask;
#endif

struct memblock_region pkvm_memory[PKVM_MEMBLOCK_REGIONS];
unsigned int pkvm_memblock_nr;
