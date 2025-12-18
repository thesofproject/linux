// SPDX-License-Identifier: GPL-2.0
#include <linux/kvm_types.h>
#include <linux/memblock.h>
#include <kvm_emulate.h>
#include <vmx/x86_ops.h>
#include "host_vmx.h"

#define CR4			4
#define MOV_TO_CR		0

struct vmcs_config host_vmcs_config;

static void skip_emulated_instruction(void)
{
	unsigned long rip;

	rip = vmcs_readl(GUEST_RIP);
	rip += vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	vmcs_writel(GUEST_RIP, rip);
}

static void handle_cpuid(struct kvm_vcpu *vcpu)
{
	u32 eax, ebx, ecx, edx;

	eax = vcpu->arch.regs[VCPU_REGS_RAX];
	ecx = vcpu->arch.regs[VCPU_REGS_RCX];
	native_cpuid(&eax, &ebx, &ecx, &edx);
	vcpu->arch.regs[VCPU_REGS_RAX] = eax;
	vcpu->arch.regs[VCPU_REGS_RBX] = ebx;
	vcpu->arch.regs[VCPU_REGS_RCX] = ecx;
	vcpu->arch.regs[VCPU_REGS_RDX] = edx;
}

static void handle_cr(struct kvm_vcpu *vcpu)
{
	struct vcpu_vt *vt = to_vt(vcpu);
	unsigned long exit_qual, val;
	int cr, type, reg;

	exit_qual = vt->exit_qualification;
	cr = exit_qual & 15;
	type = (exit_qual >> 4)	& 3;
	reg = (exit_qual >> 8) & 15;

	switch (type) {
	case MOV_TO_CR:
		switch (cr) {
		case CR4:
			/*
			 * VMXE bit is owned by pkvm, others are owned by host
			 * So only when guest is trying to modify VMXE bit it
			 * can cause vmexit and get here.
			 */
			val = vcpu->arch.regs[reg];
			vmcs_writel(CR4_READ_SHADOW, val);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static bool is_msr_in_bitmap_range(unsigned long msr)
{
	return msr <= 0x1FFF || (msr >= 0xC0000000 && msr <= 0xC0001FFF);
}

static int handle_read_msr(struct kvm_vcpu *vcpu)
{
	unsigned long msr = vcpu->arch.regs[VCPU_REGS_RCX];
	u32 low, high;

	/*
	 * The MSR reading bitmap doesn't intercept any MSR. If the vmexit is
	 * caused by such MSR in the range of the bitmap, it should be a code
	 * bug.
	 */
	BUG_ON(is_msr_in_bitmap_range(msr));

	if (rdmsr_safe(msr, &low, &high)) {
		kvm_inject_gp(vcpu, 0);
		return X86EMUL_UNHANDLEABLE;
	}

	vcpu->arch.regs[VCPU_REGS_RAX] = low;
	vcpu->arch.regs[VCPU_REGS_RDX] = high;

	return X86EMUL_CONTINUE;
}

static int handle_write_msr(struct kvm_vcpu *vcpu)
{
	unsigned long msr = vcpu->arch.regs[VCPU_REGS_RCX];
	int ret = X86EMUL_CONTINUE;
	u32 low, high;

	low = vcpu->arch.regs[VCPU_REGS_RAX];
	high = vcpu->arch.regs[VCPU_REGS_RDX];

	switch (msr) {
	default:
		/*
		 * The MSRs intercepted by the writing bitmap should be
		 * emulated by the switch cases. Otherwise it should be a code
		 * bug.
		 */
		BUG_ON(is_msr_in_bitmap_range(msr));

		if (wrmsr_safe(msr, low, high))
			ret = X86EMUL_UNHANDLEABLE;

		break;
	}

	if (ret == X86EMUL_UNHANDLEABLE)
		kvm_inject_gp(vcpu, 0);

	return ret;
}

static void handle_xsetbv(struct kvm_vcpu *vcpu)
{
	u32 eax = (u32)(vcpu->arch.regs[VCPU_REGS_RAX] & -1u);
	u32 edx = (u32)(vcpu->arch.regs[VCPU_REGS_RDX] & -1u);
	u32 ecx = (u32)(vcpu->arch.regs[VCPU_REGS_RCX] & -1u);

	asm volatile(".byte 0x0f,0x01,0xd1"
			: : "a" (eax), "d" (edx), "c" (ecx));
}

static void handle_pending_events(struct kvm_vcpu *vcpu)
{
	if (kvm_check_request(KVM_REQ_EVENT, vcpu)) {
		if (vcpu->arch.exception.pending) {
			vmx_inject_exception(vcpu);
			vcpu->arch.exception.pending = false;
			vcpu->arch.exception.injected = true;
		}
	}
}

void pkvm_host_vmexit_main(struct vcpu_vmx *vmx)
{
	struct kvm_vcpu *vcpu = &vmx->vcpu;
	struct vcpu_vt *vt = &vmx->vt;
	bool skip_instruction = false;

	vcpu->arch.cr2 = native_read_cr2();
	vcpu->arch.exception.injected = false;

	vt->exit_reason.full = vmcs_read32(VM_EXIT_REASON);
	vt->exit_qualification = vmcs_readl(EXIT_QUALIFICATION);

	switch (vt->exit_reason.full) {
	case EXIT_REASON_CPUID:
		handle_cpuid(vcpu);
		skip_instruction = true;
		break;
	case EXIT_REASON_CR_ACCESS:
		handle_cr(vcpu);
		skip_instruction = true;
		break;
	case EXIT_REASON_MSR_READ:
		if (handle_read_msr(vcpu) == X86EMUL_CONTINUE)
			skip_instruction = true;
		break;
	case EXIT_REASON_MSR_WRITE:
		if (handle_write_msr(vcpu) == X86EMUL_CONTINUE)
			skip_instruction = true;
		break;
	case EXIT_REASON_XSETBV:
		handle_xsetbv(vcpu);
		skip_instruction = true;
		break;
	default:
		break;
	}

	if (skip_instruction)
		skip_emulated_instruction();

	handle_pending_events(vcpu);

	if (vcpu->arch.cr2 != native_read_cr2())
		native_write_cr2(vcpu->arch.cr2);
}
