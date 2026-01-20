/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KVM_PKVM_H
#define _ASM_X86_KVM_PKVM_H

#ifdef CONFIG_PKVM_X86
#include <linux/mm.h>

#define PKVM_MEMBLOCK_REGIONS		128
#define PKVM_STACK_SIZE			SZ_16K

struct pkvm_pcpu {
	u8 stack[PKVM_STACK_SIZE] __aligned(16);
};

struct pkvm_hyp {
	int num_cpus;
	struct pkvm_pcpu *pcpus[CONFIG_NR_CPUS];
};

#define PKVM_HYP_PAGES		(PAGE_ALIGN(sizeof(struct pkvm_hyp)) >> PAGE_SHIFT)
#define PKVM_PCPU_PAGES		(PAGE_ALIGN(sizeof(struct pkvm_pcpu)) >> PAGE_SHIFT)

u64 pkvm_total_reserve_pages(void);
void *pkvm_early_alloc_contig(unsigned int nr_pages);
void pkvm_early_alloc_init(void *virt, unsigned long size);

static inline unsigned long pkvm_data_pages(void)
{
	return PKVM_HYP_PAGES + PKVM_PCPU_PAGES * num_possible_cpus();
}

#endif /* CONFIG_PKVM_X86 */

#endif /* _ASM_X86_KVM_PKVM_H */
