/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PKVM_HC
BUILD_BUG_ON(1)
#endif

#ifndef PKVM_HC_OUT
#define PKVM_HC_OUT PKVM_HC
#endif

/* Hypercalls used only during pKVM initialization */
PKVM_HC(init)
PKVM_HC(init_finalize)
PKVM_HC(reprivilege_cpu)

/* pKVM vmexit tracing/profiling */
PKVM_HC(enable_vmexit_trace)
PKVM_HC(dump_vmexit_trace)

/* KVM ops */
PKVM_HC(check_processor_compatibility)
PKVM_HC(enable_virtualization_cpu)
PKVM_HC(vm_init)
PKVM_HC_OUT(vm_destroy)
PKVM_HC(vcpu_create)
PKVM_HC_OUT(vcpu_free)
PKVM_HC(vcpu_load)
PKVM_HC(vcpu_put)
PKVM_HC(update_exception_bitmap)
PKVM_HC(set_efer)
PKVM_HC(set_msr)
PKVM_HC_OUT(get_msr)

#undef PKVM_HC
#undef PKVM_HC_OUT
