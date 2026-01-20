/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KVM_PKVM_H
#define _ASM_X86_KVM_PKVM_H

#ifdef CONFIG_PKVM_X86
#include <linux/bug.h>
#include <linux/mm.h>
#include <asm/pkvm_image.h>

#define PKVM_MEMBLOCK_REGIONS		128
#define PKVM_STACK_SIZE			SZ_16K
/* Size of reserved space for private parameter in pKVM stack */
#define PKVM_STACK_TOP_RESV		16

struct pkvm_pcpu {
	u8 stack[PKVM_STACK_SIZE] __aligned(16);
};

struct pkvm_hyp {
	int num_cpus;
	struct pkvm_pcpu *pcpus[CONFIG_NR_CPUS];
	struct kvm *host_kvm;
	struct kvm_vcpu *host_vcpus[CONFIG_NR_CPUS];
};

#define PKVM_HYP_PAGES		(PAGE_ALIGN(sizeof(struct pkvm_hyp)) >> PAGE_SHIFT)
#define PKVM_PCPU_PAGES		(PAGE_ALIGN(sizeof(struct pkvm_pcpu)) >> PAGE_SHIFT)

u64 pkvm_total_reserve_pages(void);
PKVM_DECLARE(void *, pkvm_early_alloc_page, (void));
PKVM_DECLARE(void *, pkvm_early_alloc_contig, (unsigned int nr_pages));
PKVM_DECLARE(void, pkvm_early_alloc_init, (void *virt, unsigned long size));

static inline unsigned long pkvm_data_pages(unsigned long extra_global,
					    unsigned long extra_percpu)
{
	unsigned long global_pages = PKVM_HYP_PAGES + extra_global;
	unsigned long percpu_pages = PKVM_PCPU_PAGES + extra_percpu;

	return global_pages + percpu_pages * num_possible_cpus();
}

static inline unsigned long get_host_stack_top(struct pkvm_pcpu *pcpu)
{
	return (unsigned long) &pcpu->stack[sizeof(pcpu->stack)];
}

#ifdef __PKVM_HYP__

#undef WARN_ON
#undef WARN
#undef WARN_ON_ONCE
#undef WARN_ONCE
#undef _BUG_FLAGS

#define WARN_ON(condition) ({						\
	int __ret_warn_on = !!(condition);				\
	unlikely(__ret_warn_on);					\
})

#define WARN(condition, format...) ({					\
	int __ret_warn_on = !!(condition);				\
	no_printk(format);						\
	unlikely(__ret_warn_on);					\
})

#define WARN_ON_ONCE(condition) WARN_ON(condition)
#define WARN_ONCE(condition, format...) WARN(condition, format)

#define _BUG_FLAGS(ins, flags, extra)  asm volatile(ins)

#endif /* __PKVM_HYP__ */

#endif /* CONFIG_PKVM_X86 */

#endif /* _ASM_X86_KVM_PKVM_H */
