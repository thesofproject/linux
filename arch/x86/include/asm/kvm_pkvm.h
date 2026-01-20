/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KVM_PKVM_H
#define _ASM_X86_KVM_PKVM_H

#ifdef CONFIG_PKVM_X86

#define PKVM_MEMBLOCK_REGIONS		128

u64 pkvm_total_reserve_pages(void);

static inline unsigned long pkvm_data_pages(void)
{
	return 0;
}

#endif /* CONFIG_PKVM_X86 */

#endif /* _ASM_X86_KVM_PKVM_H */
