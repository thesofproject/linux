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

#undef PKVM_HC
