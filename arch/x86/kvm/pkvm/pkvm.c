// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include "init.h"
#include "pkvm.h"

/*
 * Needed by kvm_spurious_fault() which is a generic fault function for the
 * vendor operations, e.g., vmx ops or svm ops. The pKVM hypervisor doesn't
 * have the knowledge about the platform reboot or shutdown, so kvm_rebooting
 * is always false in the pKVM hypervisor.
 */
__visible bool kvm_rebooting;

struct pkvm_hyp *pkvm_hyp;
DEFINE_PER_CPU(struct pkvm_pcpu *, phys_cpu);
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);

void pkvm_handle_host_hypercall(struct kvm_vcpu *vcpu)
{
	int ret = 0;

	switch (pkvm_hc(vcpu)) {
	case __pkvm__init:
		ret = pkvm_init();
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pkvm_hc_set_ret(vcpu, ret);
}
