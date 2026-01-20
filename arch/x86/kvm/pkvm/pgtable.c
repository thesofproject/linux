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
