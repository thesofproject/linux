// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/kvm_host.h>
#include <vdso/page.h>
#include "memory.h"
#include "pgtable.h"

static void *pgtable_alloc_page(const struct pkvm_pgtable_mm_ops *mm_ops)
{
	return mm_ops->zalloc_page();
}

struct pgt_walk_data {
	struct pkvm_pgtable *pgt;
	unsigned long addr;
	const unsigned long start;
	const unsigned long end;
	struct pkvm_pgtable_walker *walker;
};

static int _pgtable_walk(struct pgt_walk_data *data, void *ptep, int level);
static int pgtable_visit(struct pgt_walk_data *data, void *ptep, int level)
{
	const struct pkvm_pgtable_ops *pgt_ops = data->pgt->pgt_ops;
	struct pkvm_pgtable_walker *walker = data->walker;
	bool leaf = pgt_ops->pte_is_leaf(ptep, level);
	unsigned long walk_flags = walker->walk_flags;
	struct pkvm_pgtable_visit_ctx ctx = {
		.pgt = data->pgt,
		.start = data->start,
		.end = data->end,
		.addr = data->addr,
		.level = level,
		.ptep = ptep,
	};
	void *child_ptep;
	int ret = 0;

	if (!leaf && (walk_flags & PKVM_PGTABLE_WALK_TABLE_PRE))
		ret = walker->cb(&ctx, PKVM_PGTABLE_WALK_TABLE_PRE, walker->arg);

	if (leaf && (walk_flags & PKVM_PGTABLE_WALK_LEAF)) {
		ret = walker->cb(&ctx, PKVM_PGTABLE_WALK_LEAF, walker->arg);
		/*
		 * The ptep may be updated by the walker->cb. Revisit the ptep
		 * to see if it is still leaf or not.
		 */
		leaf = pgt_ops->pte_is_leaf(ptep, level);
	}

	if (ret)
		return ret;

	if (leaf) {
		unsigned long size = pgt_ops->level_to_size(level);

		data->addr = ALIGN_DOWN(data->addr, size);
		data->addr += size;
		return ret;
	}

	child_ptep = __pkvm_va(pgt_ops->pte_to_phys(ptep));
	ret = _pgtable_walk(data, child_ptep, level - 1);
	if (ret)
		return ret;

	if (walk_flags & PKVM_PGTABLE_WALK_TABLE_POST)
		ret = walker->cb(&ctx, PKVM_PGTABLE_WALK_TABLE_POST, walker->arg);

	return ret;
}

static int _pgtable_walk(struct pgt_walk_data *data, void *ptep, int level)
{
	const struct pkvm_pgtable_ops *pgt_ops = data->pgt->pgt_ops;
	int idx = pgt_ops->vaddr_to_index(data->addr, level);
	int entry_size = pgt_ops->pte_size(level);
	int entries = pgt_ops->pte_count(level);
	int ret;

	for (; idx < entries; idx++) {
		if (data->addr >= data->end)
			break;

		ret = pgtable_visit(data, (ptep + idx * entry_size), level);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * pkvm_pgtable_init() - Initialize a pKVM page table.
 * @pgt:	The page table to be initialized.
 * @cap:	The capability to initialize this page table.
 * @mm_ops:	The memory management ops for this page table.
 * @pgt_ops:	The page table ops for this page table.
 *
 * Return: 0 on success, negative error code on failure.
 */
int pkvm_pgtable_init(struct pkvm_pgtable *pgt,
		      struct pkvm_pgtable_cap cap,
		      const struct pkvm_pgtable_mm_ops *mm_ops,
		      const struct pkvm_pgtable_ops *pgt_ops)
{
	void *root;

	if (!pgt || !mm_ops || !pgt_ops)
		return -EINVAL;

	root = pgtable_alloc_page(mm_ops);
	if (!root)
		return -ENOMEM;

	pgt->root_pa = __pkvm_pa(root);
	pgt->cap = cap;
	pgt->mm_ops = mm_ops;
	pgt->pgt_ops = pgt_ops;

	return 0;
}

/**
 * pkvm_pgtable_walk() - Walk a pKVM page table
 * @pgt:	The page table to walk.
 * @vaddr:	The virtual address for the start of the walk.
 * @size:	The walk range size.
 * @walker:	The callback descriptor which will be executed
 *		during the walk.
 *
 * The walked range is the minimum PAGE_SIZE-aligned range covering
 * [@vaddr, @vaddr + @size).
 *
 * Each page table entry within the memory range [@vaddr, @vaddr + @size) is
 * visited during the walk, including present leaves, non-present leaves, and
 * present non-leaves. The callback in @walker is executed on leaves based on
 * the walk_flags in @walker. If the callback returns zero, the walk will
 * continue to the next entry. If the callback returns a non-zero, the walk will
 * be terminated immediately and return that value.
 *
 * Return: 0 on success, negative error code on failure.
 */
int pkvm_pgtable_walk(struct pkvm_pgtable *pgt, unsigned long vaddr,
		      unsigned long size, struct pkvm_pgtable_walker *walker)
{
	struct pgt_walk_data data = {
		.pgt = pgt,
		.addr = ALIGN_DOWN(vaddr, PAGE_SIZE),
		.start = ALIGN_DOWN(vaddr, PAGE_SIZE),
		.end = ALIGN(vaddr + size, PAGE_SIZE),
		.walker = walker,
	};

	if (!pgt->root_pa)
		return -EINVAL;

	if (data.start == data.end)
		return 0;

	return _pgtable_walk(&data, __pkvm_va(pgt->root_pa), pgt->cap.level);
}
