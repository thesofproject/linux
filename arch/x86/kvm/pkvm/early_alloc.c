// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <asm/kvm_pkvm.h>
#include <asm/string.h>

static unsigned long base;
static unsigned long end;
static unsigned long cur;

void *pkvm_early_alloc_contig(unsigned int nr_pages)
{
	unsigned long size = (nr_pages << PAGE_SHIFT);
	void *ret;

	if (!nr_pages)
		return NULL;

	if (end - cur < size)
		return NULL;

	ret = (void *)cur;
	cur += size;

	memset(ret, 0, size);

	return ret;
}

void pkvm_early_alloc_init(void *virt, unsigned long size)
{
	base = cur = (unsigned long)virt;
	end = base + size;
}
