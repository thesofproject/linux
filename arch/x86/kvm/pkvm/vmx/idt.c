// SPDX-License-Identifier: GPL-2.0
#include <asm/ptrace.h>
#include <asm/trapnr.h>
#include <vmx/vmx.h>
#include <vmx/x86_ops.h>
#include "host_vmx.h"
#include "idt.h"
#include "pkvm.h"

static void handle_nmi(struct pt_regs *regs, int vector, bool has_error_code)
{
	struct kvm_vcpu *vcpu = this_cpu_read(host_vcpu);

	/*
	 * The NMI happens while the pKVM hypervisor is running, but it should
	 * be handled by the host. Record a pending NMI to be injected to the
	 * host.
	 */
	atomic_inc(&vcpu->arch.nmi_queued);
	kvm_make_request(KVM_REQ_NMI, vcpu);

	/*
	 * Request host immediate exit in case the pending NMI has already been
	 * handled in this host vmexit handling cycle.
	 */
	request_host_immediate_exit(to_vmx(vcpu));
}

void pkvm_vmx_register_excp_handlers(void)
{
	pkvm_register_excp_handler(X86_TRAP_NMI, handle_nmi);
}
