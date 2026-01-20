/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_VMX_HOST_VMX_H
#define __PKVM_VMX_HOST_VMX_H

#include <vmx/vmx.h>

void pkvm_host_vmexit_main(struct vcpu_vmx *vmx);

#endif /* __PKVM_VMX_HOST_VMX_H */
