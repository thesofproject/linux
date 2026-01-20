/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_MMU_H
#define __PKVM_X86_MMU_H

int pkvm_hyp_mmu_init(void *pool_base, unsigned long pool_pages);

#endif /* __PKVM_X86_MMU_H */
