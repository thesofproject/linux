/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PKVM_X86_MEMORY_H
#define __PKVM_X86_MEMORY_H

#include <linux/types.h>
#include <linux/range.h>
#include <linux/mm.h>
#include <vdso/limits.h>
#include <asm/page.h>
#include "mem_protect.h"

#define __pkvm_pa		__pa
#define __pkvm_va		__va

struct pkvm_page {
	unsigned short refcount;
	u8 order;

	/* Store host memory page state. */
	enum pkvm_page_state host_state: 8;
};

/*
 * Make sure pkvm_page->host_state is large enough to store enum
 * pkvm_page_state.
 */
static_assert(PKVM_PAGE_STATE_BITS <= 8);

extern u64 __pkvm_vmemmap;
#define pkvm_vmemmap ((struct pkvm_page *)__pkvm_vmemmap)

#define pkvm_phys_to_pfn(phys)	((phys) >> PAGE_SHIFT)
#define pkvm_pfn_to_phys(pfn)	((phys_addr_t)((pfn) << PAGE_SHIFT))
#define pkvm_phys_to_page(phys)	(&pkvm_vmemmap[pkvm_phys_to_pfn(phys)])
#define pkvm_virt_to_page(virt)	pkvm_phys_to_page(__pkvm_pa(virt))
#define pkvm_virt_to_pfn(virt)	pkvm_phys_to_pfn(__pkvm_pa(virt))

#define pkvm_page_to_pfn(page)	((struct pkvm_page *)(page) - pkvm_vmemmap)
#define pkvm_page_to_phys(page)  pkvm_pfn_to_phys((pkvm_page_to_pfn(page)))
#define pkvm_page_to_virt(page)	__pkvm_va(pkvm_page_to_phys(page))
#define pkvm_page_to_pool(page)	(((struct pkvm_page *)page)->pool)

/*
 * Refcounting for 'struct pkvm_page'.
 * pkvm_pool::lock must be held if atomic access to the refcount is required.
 */
static inline int pkvm_page_count(void *addr)
{
	struct pkvm_page *p = pkvm_virt_to_page(addr);

	return p->refcount;
}

static inline void pkvm_page_ref_inc(struct pkvm_page *p)
{
	BUG_ON(p->refcount == USHRT_MAX);
	p->refcount++;
}

static inline void pkvm_page_ref_dec(struct pkvm_page *p)
{
	BUG_ON(!p->refcount);
	p->refcount--;
}

static inline int pkvm_page_ref_dec_and_test(struct pkvm_page *p)
{
	pkvm_page_ref_dec(p);
	return (p->refcount == 0);
}

static inline void pkvm_set_page_refcounted(struct pkvm_page *p)
{
	BUG_ON(p->refcount);
	p->refcount = 1;
}

bool pkvm_find_addr_range(unsigned long phys, struct range *range);

static inline bool is_memory_range(unsigned long phys, unsigned long size)
{
	struct range target = {
		.start = PAGE_ALIGN_DOWN(phys),
		.end = PAGE_ALIGN(phys + size) - 1,
	};
	struct range range;

	if (!pkvm_find_addr_range(phys, &range))
		return false;

	return range_contains(&range, &target);
}

#endif /* __PKVM_X86_MEMORY_H */
