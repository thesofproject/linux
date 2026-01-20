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

static bool leaf_in_addr_range(unsigned long leaf_size,
			       unsigned long addr,
			       unsigned long end)
{
	if (!IS_ALIGNED(addr, leaf_size))
		return false;

	if (leaf_size > (end - addr))
		return false;

	return true;
}

struct pgt_map_data {
	unsigned long phys;
	u64 prot;
};

static bool leaf_mapping_changed(struct pkvm_pgtable_visit_ctx *ctx,
				 struct pgt_map_data *data)
{
	const struct pkvm_pgtable_ops *pgt_ops = ctx->pgt->pgt_ops;
	unsigned long leaf_size, mapped_phys, new_phys;

	/* Property bits are changed */
	if (data->prot != pgt_ops->pte_to_prot(ctx->ptep))
		return true;

	leaf_size = pgt_ops->level_to_size(ctx->level);
	mapped_phys = pgt_ops->pte_to_phys(ctx->ptep);
	new_phys = data->phys + (ctx->addr - ctx->start);
	/*
	 * The new physical address is not covered by the old mapped range
	 * [mapped_phys, mapped_phys + leaf_size).
	 */
	if (!((new_phys >= mapped_phys) && new_phys < (mapped_phys + leaf_size)))
		return true;

	return false;
}

static bool leaf_mapping_allowed(struct pkvm_pgtable_visit_ctx *ctx,
				 struct pgt_map_data *data)
{
	unsigned long leaf_size = ctx->pgt->pgt_ops->level_to_size(ctx->level);

	if (!leaf_in_addr_range(leaf_size, ctx->addr, ctx->end))
		return false;

	if (!((1 << ctx->level) & ctx->pgt->cap.allowed_pgsz))
		return false;

	return IS_ALIGNED(data->phys + (ctx->addr - ctx->start), leaf_size);
}

static void pgtable_split(struct pkvm_pgtable_visit_ctx *ctx, void *child_ptep)
{
	const struct pkvm_pgtable_mm_ops *mm_ops = ctx->pgt->mm_ops;
	const struct pkvm_pgtable_ops *pgt_ops = ctx->pgt->pgt_ops;
	unsigned long phys, phys_end, step_size;
	int i = 0, entry_size, child_level;
	u64 prot;

	BUG_ON(ctx->level <= PG_LEVEL_4K);

	/* Reuse the large mapping's prot. */
	prot = pgt_ops->pte_to_prot(ctx->ptep);
	child_level = ctx->level - 1;
	if (child_level > PG_LEVEL_4K)
		pgt_ops->pte_mkhuge(&prot);

	phys = pgt_ops->pte_to_phys(ctx->ptep);
	phys_end = phys + pgt_ops->level_to_size(ctx->level);
	step_size = pgt_ops->level_to_size(child_level);
	entry_size = pgt_ops->pte_size(child_level);

	for (i = 0; phys < phys_end; phys += step_size, i++) {
		pgt_ops->pte_set(child_ptep + i * entry_size, phys | prot);
		mm_ops->get_page(child_ptep);
	}
}

static int pgtable_try_map_leaf(struct pkvm_pgtable_visit_ctx *ctx,
				struct pgt_map_data *data)
{
	const struct pkvm_pgtable_mm_ops *mm_ops = ctx->pgt->mm_ops;
	const struct pkvm_pgtable_ops *pgt_ops = ctx->pgt->pgt_ops;
	bool flush_tlb, was_present, is_present;
	void *ptep = ctx->ptep;
	u64 new, old;

	if (!leaf_mapping_changed(ctx, data))
		return 0;

	if (!leaf_mapping_allowed(ctx, data)) {
		/*
		 * 4K mapping should always be allowed, since the start and end
		 * addresses are aligned to PAGE_SIZE.
		 */
		BUG_ON(ctx->level == PG_LEVEL_4K);
		/*
		 * For the other levels, goes down to the next level to try
		 * if the mapping would be allowed by returning -E2BIG.
		 */
		return -E2BIG;
	}

	was_present = pgt_ops->pte_present(ptep);
	new = (data->phys + (ctx->addr - ctx->start)) | data->prot;
	old = pgt_ops->pte_to_phys(ptep) | pgt_ops->pte_to_prot(ptep);
	/*
	 * Need to flush TLB when the previous mapping was present and the new
	 * mapping changes either physical address or property bits. Otherwise
	 * no need to flush TLB.
	 */
	flush_tlb = was_present && new != old;

	if (ctx->level != PG_LEVEL_4K)
		pgt_ops->pte_mkhuge(&new);

	pgt_ops->pte_set(ptep, new);

	is_present = pgt_ops->pte_present(ptep);
	/*
	 * Need to increment the page table page refcnt when installs a present
	 * leaf if the refcnt was not already incremented by the previous one.
	 *
	 * Similarly need to decrement the page table page refcnt when removes a
	 * present leaf if the refcnt was incremented by the previous one.
	 */
	if (is_present && !was_present)
		mm_ops->get_page(ptep);
	else if (!is_present && was_present)
		mm_ops->put_page(ptep);

	if (flush_tlb)
		pgt_ops->flush_tlb(ctx->pgt, ctx->addr,
				   pgt_ops->level_to_size(ctx->level));

	return 0;
}

static int __map_walker(struct pkvm_pgtable_visit_ctx *ctx,
			struct pgt_map_data *data)
{
	const struct pkvm_pgtable_mm_ops *mm_ops = ctx->pgt->mm_ops;
	const struct pkvm_pgtable_ops *pgt_ops = ctx->pgt->pgt_ops;
	void *ptep = ctx->ptep;
	void *page;
	int ret;

	/* Try to create leaf page mapping on current level */
	ret = pgtable_try_map_leaf(ctx, data);
	if (ret != -E2BIG)
		return ret;

	/*
	 * The mapping needs be done at the next level or even smaller.
	 * Allocate a new page as the next level page table page.
	 */
	page = pgtable_alloc_page(mm_ops);
	if (!page)
		return -ENOMEM;

	/*
	 * If a huge mapping already exists at the current level, split it into
	 * smaller ones.
	 */
	if (pgt_ops->pte_huge(ptep)) {
		/* Split doesn't change the translation so no need to flush tlb. */
		mm_ops->put_page(ptep);
		pgtable_split(ctx, page);
	}

	mm_ops->get_page(ptep);
	pgt_ops->pte_set(ptep, ctx->pgt->cap.table_prot | __pkvm_pa(page));

	return 0;
}

/* TODO: Support merging small entries into a huge entry. */
static int map_walker(struct pkvm_pgtable_visit_ctx *ctx, unsigned long walk_flags,
		      void *const arg)
{
	struct pgt_map_data *data = arg;

	switch (walk_flags) {
	case PKVM_PGTABLE_WALK_LEAF:
		return __map_walker(ctx, data);
	case PKVM_PGTABLE_WALK_TABLE_PRE:
	case PKVM_PGTABLE_WALK_TABLE_POST:
		break;
	}

	return -EINVAL;
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

/**
 * pkvm_pgtable_map() - Install virtual addr to physical addr mappings in a pkvm
 *			pgtable.
 * @pgt:	The page table to install mappings.
 * @vaddr:	The virtual address of the installed mapping.
 * @phys:	The physical address to map.
 * @size:	The memory size to map.
 * @prot:	The property bits to create the mapping.
 *
 * The mapped range is the minimum PAGE_SIZE-aligned range covering
 * [@vaddr, @vaddr + @size), and @phys is aligned down to the PAGE_SIZE.
 *
 * The mappings for @vaddr to @phys in [@vaddr, @vaddr + @size) are installed
 * to the page table, with the same @prot. If the mapping already exists at a
 * higher level, the huge mapping will be split into smaller ones and a new
 * mapping will be installed at the proper level. If the mapping already exists
 * at the same level but with different physical address or property bits, it
 * will be replaced with the new one and TLB will be flushed.
 *
 * Return: 0 on success, negative error code on failure.
 */
int pkvm_pgtable_map(struct pkvm_pgtable *pgt, unsigned long vaddr,
		     unsigned long phys, unsigned long size, u64 prot)
{
	struct pgt_map_data data = {
		.prot = prot,
	};
	struct pkvm_pgtable_walker walker = {
		.cb = map_walker,
		.arg = &data,
		.walk_flags = PKVM_PGTABLE_WALK_LEAF,
	};

	if (!VALID_PAGE(phys))
		return -EINVAL;

	data.phys = ALIGN_DOWN(phys, PAGE_SIZE);
	return pkvm_pgtable_walk(pgt, vaddr, size, &walker);
}
