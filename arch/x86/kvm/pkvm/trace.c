// SPDX-License-Identifier: GPL-2.0
#include <asm/pkvm_trace.h>
#include <asm/tsc.h>
#include "pkvm.h"
#include "trace.h"

struct perf_ctrl {
	bool on;
};
static DEFINE_PER_CPU(struct vmexit_perf, hvcpu_perf);
static DEFINE_PER_CPU(struct perf_ctrl, perf_ctrl);

static inline struct vmexit_perf *vcpu_to_perf(struct kvm_vcpu *vcpu)
{
	/*
	 * TODO: Currently there is only host vCPU but not guest vCPU.
	 * Enable the trace support for guest once the pKVM hypervisor
	 * support running a guest vCPU.
	 */
	BUG_ON(this_cpu_read(host_vcpu) != vcpu);

	return this_cpu_ptr(&hvcpu_perf);
}

void pkvm_trace_vmexit_start(struct kvm_vcpu *vcpu)
{
	struct perf_ctrl *pctrl = this_cpu_ptr(&perf_ctrl);
	struct vmexit_perf *perf;

	if (!pctrl->on)
		return;

	perf = vcpu_to_perf(vcpu);

	perf->tsc = rdtsc_ordered();
}

void pkvm_trace_vmexit_end(struct kvm_vcpu *vcpu, u32 reason)
{
	struct perf_ctrl *pctrl = this_cpu_ptr(&perf_ctrl);
	struct vmexit_perf *perf;
	unsigned long long cycles;

	if (!pctrl->on)
		return;

	if (reason >= MAX_EXIT_REASONS)
		return;

	perf = vcpu_to_perf(vcpu);

	cycles = rdtsc_ordered() - perf->tsc;

	pkvm_spin_lock(&perf->lock);

	perf->data.vmexit_reasons[reason].count++;
	perf->data.vmexit_reasons[reason].cycles += cycles;

	pkvm_spin_unlock(&perf->lock);
}

void pkvm_vcpu_perf_init(struct kvm_vcpu *vcpu)
{
	struct vmexit_perf *perf = vcpu_to_perf(vcpu);

	perf->data.vcpu_id = vcpu->vcpu_id;

	pkvm_spin_lock_init(&perf->lock);
}
