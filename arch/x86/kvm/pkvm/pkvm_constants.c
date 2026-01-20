// SPDX-License-Identifier: GPL-2.0
#include <linux/kbuild.h>
#include "memory.h"

int main(void)
{
	DEFINE(PKVM_VMEMMAP_ENTRY_SIZE, sizeof(struct pkvm_page));
	return 0;
}
