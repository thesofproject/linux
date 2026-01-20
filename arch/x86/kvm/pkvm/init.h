/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_INIT_H
#define __PKVM_X86_INIT_H

#include <asm/kvm_pkvm.h>

int pkvm_init(struct pkvm_mem_info infos[], int nr_info);

#endif /* __PKVM_X86_INIT_H */
