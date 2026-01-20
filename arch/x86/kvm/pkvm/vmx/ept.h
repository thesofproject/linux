/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_VMX_EPT_H
#define __PKVM_VMX_EPT_H

#include "pgtable.h"

int pkvm_host_ept_init(struct pkvm_pgtable *pgt, void *pool_base,
		       unsigned long pool_pages);

#endif /* __PKVM_VMX_EPT_H */
