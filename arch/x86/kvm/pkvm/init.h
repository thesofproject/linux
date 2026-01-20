/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_INIT_H
#define __PKVM_X86_INIT_H

#include <asm/kvm_pkvm.h>

/**
 * pkvm_init_ops - The platform vendor specific pKVM init operations used by the
 *		   pkvm_init.
 */
struct pkvm_init_ops {};

int pkvm_init(struct pkvm_mem_info infos[], int nr_info);

#endif /* __PKVM_X86_INIT_H */
