/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KVM_PKVM_H
#define _ASM_X86_KVM_PKVM_H

#ifdef CONFIG_PKVM_X86
#include <linux/bug.h>
#include <linux/kvm_host.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <asm/desc.h>
#include <asm/kvm_para.h>
#include <asm/pkvm_image.h>

#define PKVM_MEMBLOCK_REGIONS		128
#define PKVM_STACK_SIZE			SZ_16K
/* Size of reserved space for private parameter in pKVM stack */
#define PKVM_STACK_TOP_RESV		16
#define PKVM_PGTABLE_MAX_LEVELS		5

struct idt_page {
	gate_desc idt[IDT_ENTRIES];
} __aligned(PAGE_SIZE);

struct pkvm_pcpu {
	u8 stack[PKVM_STACK_SIZE] __aligned(16);
	struct gdt_page gdt_page;
	struct idt_page idt_page;
	struct tss_struct tss;
};

struct pkvm_hyp {
	int num_cpus;
	struct pkvm_pcpu *pcpus[CONFIG_NR_CPUS];
	struct kvm *host_kvm;
	struct kvm_vcpu *host_vcpus[CONFIG_NR_CPUS];
};

#define PKVM_HYP_PAGES		(PAGE_ALIGN(sizeof(struct pkvm_hyp)) >> PAGE_SHIFT)
#define PKVM_PCPU_PAGES		(PAGE_ALIGN(sizeof(struct pkvm_pcpu)) >> PAGE_SHIFT)

enum pkvm_mem_type {
	PKVM_RESERVED_UNUSED_MEMORY,
	PKVM_TEXT_DATA,
};

struct pkvm_mem_info {
	enum pkvm_mem_type type;
	unsigned long va;
	unsigned long pa;
	unsigned long size;
	u64 prot;
};

#define TO_PKVM_HC(f)		CONCATENATE(__pkvm__, f)

enum pkvm_hc {
	#define PKVM_HC(f)	TO_PKVM_HC(f),
	#include <asm/pkvm_hypercalls.h>
};

#define PKVM_HC_IN_0()
#define PKVM_HC_IN_1(a1)		, "b"(a1)
#define PKVM_HC_IN_2(a1, a2)		PKVM_HC_IN_1(a1), "c"(a2)
#define PKVM_HC_IN_3(a1, a2, a3)	PKVM_HC_IN_2(a1, a2), "d"(a3)
#define PKVM_HC_IN_4(a1, a2, a3, a4)	PKVM_HC_IN_3(a1, a2, a3), "S"(a4)

#define pkvm_hypercall(f, ...)								\
({											\
	int ret;									\
	asm volatile(KVM_HYPERCALL							\
		     : "=a"(ret)							\
		     : "a"(TO_PKVM_HC(f))						\
		       CONCATENATE(PKVM_HC_IN_, COUNT_ARGS(__VA_ARGS__))(__VA_ARGS__)	\
		     : "memory");							\
	ret;										\
})

static inline unsigned long pkvm_hc(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.regs[VCPU_REGS_RAX];
}

#define DEFINE_PKVM_HC_INPUT(n, reg)							\
static inline unsigned long pkvm_hc_input##n(struct kvm_vcpu *vcpu)			\
{											\
	return vcpu->arch.regs[VCPU_REGS_##reg];					\
}

DEFINE_PKVM_HC_INPUT(1, RBX)
DEFINE_PKVM_HC_INPUT(2, RCX)
DEFINE_PKVM_HC_INPUT(3, RDX)
DEFINE_PKVM_HC_INPUT(4, RSI)

static inline void pkvm_hc_set_ret(struct kvm_vcpu *vcpu, int ret)
{
	vcpu->arch.regs[VCPU_REGS_RAX] = ret;
}

extern unsigned long pkvm_sym(page_offset_base);
extern unsigned long pkvm_sym(phys_base);
extern struct pkvm_hyp *pkvm_sym(pkvm_hyp);
extern struct memblock_region pkvm_sym(pkvm_memory)[];
extern unsigned int pkvm_sym(pkvm_memblock_nr);
extern struct cpuinfo_x86 pkvm_sym(boot_cpu_data);
#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
extern phys_addr_t pkvm_sym(physical_mask);
#endif
extern pteval_t pkvm_sym(__default_kernel_pte_mask);
#ifdef CONFIG_AMD_MEM_ENCRYPT
extern u64 pkvm_sym(sme_me_mask);
#endif
extern struct pkvm_init_ops *pkvm_sym(init_ops);

u64 pkvm_total_reserve_pages(void);
PKVM_DECLARE(void *, pkvm_early_alloc_page, (void));
PKVM_DECLARE(void *, pkvm_early_alloc_contig, (unsigned int nr_pages));
PKVM_DECLARE(void, pkvm_early_alloc_init, (void *virt, unsigned long size));
PKVM_DECLARE(int, pkvm_setup_per_cpu, (int cpu, unsigned long base));
PKVM_DECLARE(unsigned int, pkvm_per_cpu_nr_pages, (void));
PKVM_DECLARE(unsigned long, pkvm_per_cpu_offset, (int cpu));
#define GEN(x, ...) PKVM_DECLARE(void, handle_exception_##x, (void));
#include <asm/GEN-for-each-exc.h>
#undef GEN

static inline unsigned long pkvm_data_pages(unsigned long extra_global,
					    unsigned long extra_percpu)
{
	unsigned long global_pages = PKVM_HYP_PAGES + extra_global;
	unsigned long percpu_pages = PKVM_PCPU_PAGES + extra_percpu +
				     pkvm_sym(pkvm_per_cpu_nr_pages)();

	return global_pages + percpu_pages * num_possible_cpus();
}

static inline unsigned long get_host_stack_top(struct pkvm_pcpu *pcpu)
{
	return (unsigned long) &pcpu->stack[sizeof(pcpu->stack)];
}

static inline unsigned long __pkvm_pgtable_max_pages(unsigned long nr_pages)
{
	unsigned long total = 0, i;

	/* Provision the worst case */
	for (i = 0; i < PKVM_PGTABLE_MAX_LEVELS; i++) {
		nr_pages = DIV_ROUND_UP(nr_pages, PTRS_PER_PTE);
		total += nr_pages;
	}

	/*
	 * For each level except the last one, may need an extra page table
	 * if the VA range is not aligned to the next level's page size.
	 * For example, range [0x1ff000, 0x201000) consists of just two
	 * 4K pages, however, not one but two page tables at the first
	 * level are required for mapping this range, since it crosses the
	 * 2M boundary.
	 */
	total += PKVM_PGTABLE_MAX_LEVELS - 1;

	return total;
}

static inline unsigned long __pkvm_pgtable_total_pages(void)
{
	unsigned long total = 0, i;

	for (i = 0; i < pkvm_sym(pkvm_memblock_nr); i++) {
		struct memblock_region *reg = &pkvm_sym(pkvm_memory)[i];

		total += __pkvm_pgtable_max_pages(reg->size >> PAGE_SHIFT);
	}

	return total;
}

static inline unsigned long pkvm_hyp_pgtable_pages(void)
{
	return __pkvm_pgtable_total_pages();
}

static inline unsigned long __vmemmap_memblock_size(struct memblock_region *reg,
						    size_t vmemmap_entry_size)
{
	unsigned long nr_pages = reg->size >> PAGE_SHIFT;
	unsigned long start, end;

	/* Translate the pfn to the vmemmap entry */
	start = (reg->base >> PAGE_SHIFT) * vmemmap_entry_size;
	end = start + nr_pages * vmemmap_entry_size;
	start = ALIGN_DOWN(start, PAGE_SIZE);
	end = ALIGN(end, PAGE_SIZE);

	return end - start;
}

static inline unsigned long pkvm_vmemmap_pages(size_t vmemmap_entry_size)
{
	unsigned long total_size = 0, i;

	for (i = 0; i < pkvm_sym(pkvm_memblock_nr); i++) {
		total_size += __vmemmap_memblock_size(&pkvm_sym(pkvm_memory)[i],
						      vmemmap_entry_size);
	}

	return total_size >> PAGE_SHIFT;
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
