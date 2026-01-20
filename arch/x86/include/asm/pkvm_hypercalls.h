/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PKVM_HC
BUILD_BUG_ON(1)
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

#undef PKVM_HC
