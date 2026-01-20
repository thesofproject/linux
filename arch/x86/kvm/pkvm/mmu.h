/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_MMU_H
#define __PKVM_X86_MMU_H

#include "init.h"

int pkvm_hyp_mmu_init(void *pool_base, unsigned long pool_pages);
int pkvm_hyp_mmu_switch_to_buddy(void *pool_base, unsigned long pool_pages);
void pkvm_hyp_mmu_load(void);
int pkvm_hyp_mmu_finalize(hyp_mmu_finalize_fn_t fn);
int pkvm_hyp_mmu_map(unsigned long vaddr, unsigned long phys,
		     unsigned long size, u64 prot);

int pkvm_host_mmu_init(void *pool_base, unsigned long pool_pages,
		       host_mmu_init_fn_t fn);

#endif /* __PKVM_X86_MMU_H */
