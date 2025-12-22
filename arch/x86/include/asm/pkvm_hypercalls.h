/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PKVM_HC
BUILD_BUG_ON(1)
#endif

#ifndef PKVM_HC_OUT
#define PKVM_HC_OUT PKVM_HC
#endif

#ifndef PKVM_HC_IN
#define PKVM_HC_IN PKVM_HC
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
PKVM_HC(vcpu_reset)
PKVM_HC(update_exception_bitmap)
PKVM_HC(set_efer)
PKVM_HC(set_msr)
PKVM_HC_OUT(get_msr)
PKVM_HC_OUT(cache_reg)
PKVM_HC(set_cr4)
PKVM_HC(set_cr0)
PKVM_HC_OUT(get_rflags)
PKVM_HC(set_rflags)
PKVM_HC(set_dr7)
PKVM_HC_IN(set_segment)
PKVM_HC_OUT(get_segment)
PKVM_HC_OUT(get_segment_base)
PKVM_HC_IN(set_idt)
PKVM_HC_OUT(get_idt)
PKVM_HC_IN(set_gdt)
PKVM_HC_OUT(get_gdt)
PKVM_HC(flush_tlb_all)
PKVM_HC(flush_tlb_current)
PKVM_HC(flush_tlb_gva)
PKVM_HC(flush_tlb_guest)
PKVM_HC(set_interrupt_shadow)
PKVM_HC_OUT(get_interrupt_shadow)

#undef PKVM_HC
#undef PKVM_HC_OUT
#undef PKVM_HC_IN
