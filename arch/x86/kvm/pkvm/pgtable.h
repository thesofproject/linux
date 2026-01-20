/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_PGTABLE_H
#define __PKVM_X86_PGTABLE_H

#include <linux/types.h>

struct pkvm_pgtable;

/**
 * struct pkvm_pgtable_mm_ops - Page table memory management callbacks.
 * @zalloc_page:	Allocate a page with clearing its contents.
 * @get_page:		Increment the reference count of a given page.
 * @put_page:		Decrement the reference count of a given page.
 * @page_count:		Get the reference count of a given page.
 */
struct pkvm_pgtable_mm_ops {
	void *(*zalloc_page)(void);
	void (*get_page)(void *vaddr);
	void (*put_page)(void *vaddr);
	int (*page_count)(void *vaddr);
};

/**
 * struct pkvm_pgtable_ops - Page table operation callbacks.
 * @pte_present:	Check if a pte is present.
 * @pte_huge:		Check if a pte is huge.
 * @pte_mkhuge:		Set huge for the given pte.
 * @pte_to_phys:	Decode the physical address from pte.
 * @pte_to_prot:	Decode the property bits from pte.
 * @calc_pte_perm:	Calculate the pte permission bits according to the
 *			read/write/exec permissions.
 * @calc_pte_memtype:	Calculate the pte memory type bits for either memory or
 *			MMIO.
 * @vaddr_to_index:	Calculate the entry index for a virtual address
 *			according to the given page table level.
 * @level_to_size:	Calculate the page size at a page table level.
 * @level_to_mask:	Calculate the page size mask at a page table level.
 * @pte_is_leaf:	Check if a pte is leaf.
 * @pte_size:		Calculate the pte size in bytes at a given page table
 *			level.
 * @pte_count:		Calculate the number of ptes at a given page table
 *			level.
 * @pte_set:		Set a pte to a given raw value.
 * @pte_get:		Get the raw value stored in the pte.
 * @flush_tlb:		Flush the TLB for a virtual address range.
 */
struct pkvm_pgtable_ops {
	bool (*pte_present)(void *ptep);
	bool (*pte_huge)(void *ptep);
	void (*pte_mkhuge)(void *ptep);
	unsigned long (*pte_to_phys)(void *ptep);
	u64 (*pte_to_prot)(void *ptep);
	u64 (*calc_pte_perm)(bool read, bool write, bool exec);
	u64 (*calc_pte_memtype)(bool mmio);
	int (*vaddr_to_index)(unsigned long vaddr, int level);
	unsigned long (*level_to_size)(int level);
	u64 (*level_to_mask)(int level);
	bool (*pte_is_leaf)(void *ptep, int level);
	int (*pte_size)(int level);
	int (*pte_count)(int level);
	void (*pte_set)(void *ptep, u64 val);
	u64 (*pte_get)(void *ptep);
	void (*flush_tlb)(struct pkvm_pgtable *pgt,
			  unsigned long vaddr, unsigned long size);
};

struct pkvm_pgtable_cap {
	int level;
	int allowed_pgsz;
	u64 table_prot;
};

struct pkvm_pgtable {
	unsigned long root_pa;
	struct pkvm_pgtable_cap cap;
	const struct pkvm_pgtable_mm_ops *mm_ops;
	const struct pkvm_pgtable_ops *pgt_ops;
};

struct pkvm_pgtable_visit_ctx {
	struct pkvm_pgtable *pgt;
	const unsigned long start;
	const unsigned long end;
	unsigned long addr;
	int level;
	void *ptep;
};

typedef int (*pgtable_visit_fn_t)(struct pkvm_pgtable_visit_ctx *ctx,
				  unsigned long walk_flags, void *const arg);

/**
 * enum pkvm_pgtable_walk_flags - Flags to control page table walk.
 * @PKVM_PGTABLE_WALK_TABLE_PRE:	Execute the callback on a non-leaf
 *					entry before visiting its children.
 * @PKVM_PGTABLE_WALK_LEAF:		Execute the callback on a leaf entry
 *					(either present or non-present).
 * @PKVM_PGTABLE_WALK_TABLE_POST:	Execute the callback on a non-leaf
 *					entry after visiting its children.
 */
enum pkvm_pgtable_walk_flags {
	PKVM_PGTABLE_WALK_TABLE_PRE	= BIT(0),
	PKVM_PGTABLE_WALK_LEAF		= BIT(1),
	PKVM_PGTABLE_WALK_TABLE_POST	= BIT(2),
};

/**
 * struct pkvm_pgtable_walker - The page table walk callback descriptor.
 * @cb:		The callback executed during the page table walking.
 * @arg:	The argument passed to the callback.
 * @walk_flags:	A bitwise-or of enum pkvm_pgtable_walk_flags to indicate the
 *		walking behaviors.
 */
struct pkvm_pgtable_walker {
	const pgtable_visit_fn_t cb;
	void *const arg;
	const unsigned long walk_flags;
};

int pkvm_pgtable_init(struct pkvm_pgtable *pgt,
		      struct pkvm_pgtable_cap cap,
		      const struct pkvm_pgtable_mm_ops *mm_ops,
		      const struct pkvm_pgtable_ops *pgt_ops);
int pkvm_pgtable_walk(struct pkvm_pgtable *pgt, unsigned long vaddr,
		      unsigned long size, struct pkvm_pgtable_walker *walker);
int pkvm_pgtable_map(struct pkvm_pgtable *pgt, unsigned long vaddr,
		     unsigned long phys, unsigned long size, u64 prot);

#endif /* __PKVM_X86_PGTABLE_H */
